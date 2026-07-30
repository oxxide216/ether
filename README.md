# Ether

Statically typed Lisp-like language, compiled to machine code using evm (backend of The E Programming Language).
Evolution of [Aether](https://github.com/oxxide216/aether).

## Cloning

Make sure to use this command, repository contains submodules

```shell
git clone --recursive https://github.com/oxxide216/ether
cd ether
```

## Building

Ether uses my custom build system [nsb](https://github.com/oxxide216/nsb).
To build it, do the following:

```shell
cc -o nsb nsb.c
./nsb -a
```
