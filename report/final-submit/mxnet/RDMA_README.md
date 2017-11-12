# Compile

Compiling mxnet requires OpenCV and OpenBLAS.

There are 3 versions of mxnet: TCP/IP(uses Ethernet), IPoIB(uses InfiniBand), RDMA. Orig mxnet uses default network device. To use IPoIB, `ps-lite/src/van.cc` line 228 should be modified.

```c++
//const char*  itf = Environment::Get()->find("DMLC_INTERFACE");
//std::string interface;
//if (itf) interface = std::string(itf);
std::string interface = std::string("ib0");//ib0 is the InfiniBand device's name in our cluster
```

To compile RDMA version, libverbs must be linked. In `Makefile` line 196

```Makefile
CFLAGS += -DMXNET_USE_DIST_KVSTORE -I$(PS_PATH)/include -I$(DEPS_PATH)/include -libverbs -lmlx4
```

There are two ps-* directory: origin `ps-lite` and rdma version `ps-rdma`. Adding `USE_RDMA=1` when compile to use rdma.

Compile command written in Makefile as a target

```makefile
#compile rdma version
cmplrmda:
	make -j $(nproc) USE_OPENCV=1 USE_BLAS=openblas USE_DIST_KVSTORE=1 USE_RDMA=1
#compile origin version
cmplorig:
	make -j $(nproc) USE_OPENCV=1 USE_BLAS=openblas USE_DIST_KVSTORE=1
```

**Attention**: before compile another version of mxnet, one should do `make clean`, otherwise all objects will be 'up to date' and everything stays the same.

# Run mxnet

* To run mxnet on python3, a little work should be done. In `python/mxnet/kvstore.py` line 392

    ```python
    check_call(_LIB.MXKVStoreSendCommmandToServers(
        self.handle, mx_uint(head), c_str(body)))

    ```

    `body` is a byte object, which has no method `encode()`, will be encoded in `c_str()`. One slight change would fix this.

    ```python
    check_call(_LIB.MXKVStoreSendCommmandToServers(
       self.handle, mx_uint(head), c_str(body.decode()))) 
    ```

* When running mnist on other networks, an error is raised that kernel size exceeded. Adding `pad=(1,1)` to this network's Pooling function as a parameter would fix this.

    ```python
    pool1 = mx.symbol.Pooling(
            data=lrn1, pool_type="max", kernel=(3, 3), stride=(2,2))
    ```

    ```python
    pool1 = mx.symbol.Pooling(
            data=lrn1, pool_type="max", kernel=(3, 3), pad=(1, 1), stride=(2,2))
    ```


We have written the test command as a target in Makefile.

```Makefile
.PHONY: runimg
runimg:
	python tools/launch.py -n 4  --launcher ssh -H hosts python \
example/image-classification/train_mnist.py --network mlp --kv-store dist_sync
```

File `hosts` must be provided to run distributed training.