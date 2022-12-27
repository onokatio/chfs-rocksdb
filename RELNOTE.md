# Release note for CHFS 2.0.0 (2022/10/21)

## New commands
- chfind - parallel find

## New environment variables
- CHFS_ASYNC_ACCESS - enable asynchronous read and write
- CHFS_BUF_SIZE - specify client buffer size.  Default is zero.

## New APIs
- chfs_seek - reposition read/write file offset
- chfs_readdir_index - read a directory in a specified server

## New features
- support pkgconfig
- support rpath
- Docker containers for developers

## Updated features
- chfs_unlink - improve performance
- change defaults.  Default chunk size is 64 KiB.

## Bug fixes
- libchfs - chfs_init returns no server when it is called more than the number of servers
- chfsd - fix memory leak when margo_get_input fails
- chfsd - fix election sometimes failing
- chfsd - introduce lock in KV backend
- chfsd - fix segfaults when shutting down

# Release note for CHFS 1.0.0 (2022/3/29)

CHFS is a parallel consistent hashing file system created instantly using node-local storages such as persistent memory and NVMe SSD.  It exploits the performance of persistent memory using persistent in-memory key-value store pmemkv.  For NVMe SSD, it uses the POSIX backend.  It supports InfiniBand verbs for high performance data access.

## How to create file system

    % eval `chfsctl [-h hostfile] [-p verbs] [-D] [-c /dev/dax0.0] [-m /mount/point] start`

This executes chfsd servers and mounts the CHFS at /mount/point on hosts specified by the hostfile.  The -p option specifies a communication protocol.  The -c option specifies a devdax device or a scratch directory on each host.

For the devdax device, -D option is required.  A pmem obj pool should be created with the layout pmemkv by `pmempool create -l pmemkv obj /dev/dax0.0`.  For user-level access, the permission of the device is modified; bad block check is disabled by `pmempool feature --disable CHECK_BAD_BLOCKS /dev/dax0.0`.

chfsctl outputs the setting of CHFS_SERVER environment variable, which is used to execute chfuse and CHFS commands.

For details, see [manual page of chfsctl](doc/chfsctl.1.md).

## Technical details

Osamu Tatebe, Kazuki Obata, Kohei Hiraga, Hiroki Ohtsuji, "[CHFS: Parallel Consistent Hashing File System for Node-local Persistent Memory](https://dl.acm.org/doi/fullHtml/10.1145/3492805.3492807)", Proceedings of the ACM International Conference on High Performance Computing in Asia-Pacific Region (HPC Asia 2022), pp.115-124, 2022