A simple Linux clone for x86.

## Screenshots

**startup**

![](screenshot/20260504-startup.png)

**oksh**

![](screenshot/20260504-oksh.png)

**vim**

![](screenshot/20260504-vim-1.png)

![](screenshot/20260504-vim-2.png)

**nano**

![](screenshot/20260504-nano.png)

**less**

![](screenshot/20260504-less.png)

**python**

![](screenshot/20260506-python-1.png)

![](screenshot/20260506-python-2.png)

![](screenshot/20260506-python-3.png)

**cmatrix**

![](screenshot/20260507-cmatrix.png)

**2048**

![](screenshot/20260507-2048.png)

## Prerequisites

Dependencies for the supported distros are maintained in text files:

- `.github/ci/ubuntu-build-packages.txt`
- `.github/ci/ubuntu-test-packages.txt`
- `.github/ci/fedora-build-packages.txt`
- `.github/ci/fedora-test-packages.txt`

Install both lists on Ubuntu with:

```
$ sudo apt-get update
$ sudo apt-get install -y $(grep -hEv '^[[:space:]]*#|^[[:space:]]*$' \
    .github/ci/ubuntu-build-packages.txt .github/ci/ubuntu-test-packages.txt)
```

Or on Fedora with:

```
$ sudo dnf install -y $(grep -hEv '^[[:space:]]*#|^[[:space:]]*$' \
    .github/ci/fedora-build-packages.txt .github/ci/fedora-test-packages.txt)
```

## Build

```
$ make
```

The default is a host build. Set `BUILD_IMAGE` to build kernel and app inside
a container:

```
$ make BUILD_IMAGE=ubuntu:26.04
```

Available images are `ubuntu:23.10`, `ubuntu:26.04`, `fedora:39`, and
`fedora:45`. The first podman build creates and caches the image under
`localhost/mario-build:<distro-tag>`.

Use `make BUILD_IMAGE=ubuntu:26.04 container-image` to rebuild that image
explicitly, or `make BUILD_IMAGE=ubuntu:26.04 container-shell` to open a
shell inside it.

Run the test suite with:

```
$ make test
```

## Run

```
$ make quick-run
```

```
$ make quick-run-gtk
```

## Debug

```
$ make quick-debug
```

```
$ make quick-debug-gtk
```

```
$ ./gdb.sh
```
