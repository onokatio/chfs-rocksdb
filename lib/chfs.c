#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <margo.h>
#include "ring_types.h"
#include "ring_list.h"
#include "ring_list_rpc.h"
#include "kv_types.h"
#include "kv_err.h"
#include "fs_types.h"
#include "fs_rpc.h"
#include "log.h"
#include "chfs.h"

static char chfs_client[PATH_MAX];
static uint32_t chfs_uid, chfs_gid;
static int chfs_chunk_size = 4096;
static int chfs_get_rdma_thresh = 2048;
static int chfs_rpc_timeout_msec = 0;	/* no timeout */

static ABT_mutex chfs_fd_mutex;
static int chfs_fd_table_size;
struct chfs_fd_table {
	char *path;
	mode_t mode;
	int chunk_size, pos;
	int ref, closed;
} *chfs_fd_table;

void
chfs_set_chunk_size(int chunk_size)
{
	log_info("chfs_set_chunk_size: %d", chunk_size);
	chfs_chunk_size = chunk_size;
}

void
chfs_set_get_rdma_thresh(int thresh)
{
	log_info("chfs_set_get_rdma_thresh: %d", thresh);
	chfs_get_rdma_thresh = thresh;
}

void
chfs_set_rpc_timeout_msec(int timeout)
{
	log_info("chfs_set_rpc_timeout_msec: %d", timeout);
	chfs_rpc_timeout_msec = timeout;
}

static void
init_fd_table()
{
	int i;

	chfs_fd_table_size = 100;
	chfs_fd_table = malloc(sizeof(*chfs_fd_table) * chfs_fd_table_size);
	assert(chfs_fd_table);

	for (i = 0; i < chfs_fd_table_size; ++i)
		chfs_fd_table[i].path = NULL;
	ABT_mutex_create(&chfs_fd_mutex);
}

static char *
margo_protocol(const char *server)
{
	int s = 0;
	char *prot;

	while (server[s] && server[s] != ':')
		++s;
	if (server[s] != ':')
		return (NULL);
	prot = malloc(s + 1);
	assert(prot);
	strncpy(prot, server, s);
	prot[s] = '\0';
	return (prot);
}

int
chfs_init(const char *server)
{
	margo_instance_id mid;
	size_t client_size = sizeof(chfs_client);
	hg_addr_t client_addr;
	char *chunk_size, *rdma_thresh, *rpc_timeout, *proto;
	char *log_priority;
	int max_log_level;
	hg_return_t ret;

	log_priority = getenv("CHFS_LOG_PRIORITY");
	if (log_priority != NULL) {
		max_log_level = log_priority_from_name(log_priority);
		if (max_log_level == -1)
			log_error("%s: invalid log priority", log_priority);
		else
			log_set_priority_max_level(max_log_level);
	}
	if (server == NULL)
		server = getenv("CHFS_SERVER");
	if (server == NULL)
		log_fatal("chfs_init: no server");
	log_info("chfs_init: server %s", server);

	chunk_size = getenv("CHFS_CHUNK_SIZE");
	if (chunk_size != NULL)
		chfs_set_chunk_size(atoi(chunk_size));

	rdma_thresh = getenv("CHFS_RDMA_THRESH");
	if (rdma_thresh != NULL)
		chfs_set_get_rdma_thresh(atoi(rdma_thresh));

	rpc_timeout = getenv("CHFS_RPC_TIMEOUT_MSEC");
	if (rpc_timeout != NULL)
		chfs_set_rpc_timeout_msec(atoi(rpc_timeout));

	proto = margo_protocol(server);
	if (proto == NULL)
		log_fatal("%s: no protocol", server);
	mid = margo_init(proto, MARGO_CLIENT_MODE, 1, 0);
	free(proto);
	if (mid == MARGO_INSTANCE_NULL)
		log_fatal("margo_init failed, abort");
	ring_list_init(NULL);
	ring_list_rpc_init(mid, chfs_rpc_timeout_msec);
	fs_client_init(mid, chfs_rpc_timeout_msec);

	margo_addr_self(mid, &client_addr);
	margo_addr_to_string(mid, chfs_client, &client_size, client_addr);
	margo_addr_free(mid, client_addr);

	init_fd_table();
	chfs_uid = getuid();
	chfs_gid = getgid();

	ret = ring_list_rpc_node_list(server);
	if (ret != HG_SUCCESS)
		log_fatal("%s: %s", server, HG_Error_to_string(ret));

	return (0);
}

int
chfs_term()
{
	return (0);
}

static int
create_fd_unlocked(const char *path, mode_t mode, int chunk_size)
{
	struct chfs_fd_table *tmp;
	int fd, i;

	for (fd = 0; fd < chfs_fd_table_size; ++fd)
		if (chfs_fd_table[fd].path == NULL)
			break;
	if (fd == chfs_fd_table_size) {
		tmp = realloc(chfs_fd_table,
			sizeof(*chfs_fd_table) * chfs_fd_table_size * 2);
		if (tmp == NULL)
			return (-1);
		chfs_fd_table = tmp;
		chfs_fd_table_size *= 2;
		for (i = fd; i < chfs_fd_table_size; ++i)
			chfs_fd_table[i].path = NULL;
	}
	chfs_fd_table[fd].path = strdup(path);
	assert(chfs_fd_table[fd].path);
	chfs_fd_table[fd].mode = mode;
	chfs_fd_table[fd].chunk_size = chunk_size;
	chfs_fd_table[fd].pos = 0;
	chfs_fd_table[fd].ref = 0;
	chfs_fd_table[fd].closed = 0;
	return (fd);
}

static int
create_fd(const char *path, mode_t mode, int chunk_size)
{
	int fd;

	ABT_mutex_lock(chfs_fd_mutex);
	fd = create_fd_unlocked(path, mode, chunk_size);
	ABT_mutex_unlock(chfs_fd_mutex);
	return (fd);
}

static void
clear_fd_table_unlocked(struct chfs_fd_table *tab)
{
	free(tab->path);
	tab->path = NULL;
}

static int
check_fd_unlocked(int fd)
{
	if (fd < 0 || fd >= chfs_fd_table_size)
		return (-1);
	if (chfs_fd_table[fd].path == NULL)
		return (-1);
	return (0);
}

static int
clear_fd_unlocked(int fd)
{
	if (check_fd_unlocked(fd))
		return (-1);
	if (chfs_fd_table[fd].ref > 0)
		chfs_fd_table[fd].closed = 1;
	else
		clear_fd_table_unlocked(&chfs_fd_table[fd]);
	return (0);
}

static int
clear_fd(int fd)
{
	int r;

	ABT_mutex_lock(chfs_fd_mutex);
	r = clear_fd_unlocked(fd);
	ABT_mutex_unlock(chfs_fd_mutex);
	return (r);
}

static struct chfs_fd_table *
get_fd_table_unlocked(int fd)
{
	if (check_fd_unlocked(fd))
		return (NULL);
	if (chfs_fd_table[fd].closed)
		return (NULL);
	++chfs_fd_table[fd].ref;
	return (&chfs_fd_table[fd]);
}

static struct chfs_fd_table *
get_fd_table(int fd)
{
	struct chfs_fd_table *tab;

	ABT_mutex_lock(chfs_fd_mutex);
	tab = get_fd_table_unlocked(fd);
	ABT_mutex_unlock(chfs_fd_mutex);
	return (tab);
}

static void
release_fd_table_unlocked(struct chfs_fd_table *tab)
{
	--tab->ref;
	if (tab->closed == 0 || tab->ref > 0)
		return;
	clear_fd_table_unlocked(tab);
}

static void
release_fd_table(struct chfs_fd_table *tab)
{
	ABT_mutex_lock(chfs_fd_mutex);
	release_fd_table_unlocked(tab);
	ABT_mutex_unlock(chfs_fd_mutex);
}

static hg_return_t
chfs_rpc_inode_create(void *key, size_t key_size, mode_t mode, int chunk_size,
	int *errp)
{
	char *target;
	hg_return_t ret;

	while (1) {
		target = ring_list_lookup(key, key_size);
		ret = fs_rpc_inode_create(target, key, key_size, chfs_uid,
			chfs_gid, mode, chunk_size, errp);
		if (ret == HG_SUCCESS)
			break;
		ring_list_remove(target);
		free(target);
	}
	free(target);
	return (ret);
}

static hg_return_t
chfs_rpc_inode_write(void *key, size_t key_size, const void *buf, size_t *size,
	size_t offset, mode_t mode, int chunk_size, int *errp)
{
	char *target;
	hg_return_t ret;

	while (1) {
		target = ring_list_lookup(key, key_size);
		ret = fs_rpc_inode_write(target, key, key_size, buf,
			size, offset, mode, chunk_size, errp);
		if (ret == HG_SUCCESS)
			break;
		ring_list_remove(target);
		free(target);
	}
	free(target);
	return (ret);
}

static hg_return_t
chfs_rpc_inode_read(void *key, size_t key_size, void *buf, size_t *size,
	size_t offset, int *errp)
{
	char *target;
	hg_return_t ret;

	while (1) {
		target = ring_list_lookup(key, key_size);
		if (*size < chfs_get_rdma_thresh)
			ret = fs_rpc_inode_read(target, key, key_size, buf,
				size, offset, errp);
		else
			ret = fs_rpc_inode_read_rdma(target, key, key_size,
				chfs_client, buf, size, offset, errp);
		if (ret == HG_SUCCESS)
			break;
		ring_list_remove(target);
		free(target);
	}
	free(target);
	return (ret);
}

static hg_return_t
chfs_rpc_remove(void *key, size_t key_size, int *errp)
{
	char *target;
	hg_return_t ret;

	while (1) {
		target = ring_list_lookup(key, key_size);
		ret = fs_rpc_inode_remove(target, key, key_size, errp);
		if (ret == HG_SUCCESS)
			break;
		ring_list_remove(target);
		free(target);
	}
	free(target);
	return (ret);
}

static hg_return_t
chfs_rpc_inode_stat(void *key, size_t key_size, struct fs_stat *st, int *errp)
{
	char *target;
	hg_return_t ret;

	while (1) {
		target = ring_list_lookup(key, key_size);
		ret = fs_rpc_inode_stat(target, key, key_size, st, errp);
		if (ret == HG_SUCCESS)
			break;
		ring_list_remove(target);
		free(target);
	}
	free(target);
	return (ret);
}

static const char *
skip_slash(const char *p)
{
	while (*p && *p == '/')
		++p;
	return (p);
}

int
chfs_create_chunk_size(const char *path, int32_t flags, mode_t mode,
	int chunk_size)
{
	const char *p = skip_slash(path);
	hg_return_t ret;
	int fd, err;

	mode |= S_IFREG;
	fd = create_fd(p, mode, chunk_size);
	if (fd < 0)
		return (-1);

	ret = chfs_rpc_inode_create((void *)p, strlen(p) + 1, mode, chunk_size,
		&err);
	if (ret == HG_SUCCESS && err == KV_SUCCESS)
		return (fd);

	clear_fd(fd);
	return (-1);
}

int
chfs_create(const char *path, int32_t flags, mode_t mode)
{
	return (chfs_create_chunk_size(path, flags, mode, chfs_chunk_size));
}

int
chfs_open(const char *path, int32_t flags)
{
	const char *p = skip_slash(path);
	struct fs_stat st;
	hg_return_t ret;
	int fd, err;

	ret = chfs_rpc_inode_stat((void *)p, strlen(p) + 1, &st, &err);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);

	fd = create_fd(p, st.mode, st.chunk_size);
	if (fd >= 0)
		return (fd);
	return (-1);
}

#define MAX_INT_SIZE 11

static void *
path_index(const char *path, int index, size_t *size)
{
	void *path_index;
	int path_len;

	if (path == NULL)
		return (NULL);
	path_len = strlen(path) + 1;
	path_index = malloc(path_len + MAX_INT_SIZE);
	if (path_index == NULL)
		return (NULL);
	strcpy(path_index, path);
	if (index == 0) {
		*size = path_len;
		return (path_index);
	}
	sprintf(path_index + path_len, "%d", index);
	*size = path_len + strlen(path_index + path_len) + 1;
	return (path_index);
}

int
chfs_fsync(int fd)
{
	return (0);
}

int
chfs_close(int fd)
{
	return (clear_fd(fd));
}

ssize_t
chfs_pwrite(int fd, const void *buf, size_t size, off_t offset)
{
	struct chfs_fd_table *tab = get_fd_table(fd);
	void *path;
	int index, local_pos, chunk_size, err;
	mode_t mode;
	size_t s = size, ss = 0, psize;
	hg_return_t ret;

	if (tab == NULL)
		return (-1);

	chunk_size = tab->chunk_size;
	mode = tab->mode;
	index = offset / chunk_size;
	local_pos = offset % chunk_size;

	if (local_pos + s > chunk_size)
		s = chunk_size - local_pos;
	assert(s > 0);

	path = path_index(tab->path, index, &psize);
	release_fd_table(tab);
	if (path == NULL)
		return (-1);
	ret = chfs_rpc_inode_write(path, psize, (void *)buf, &s, local_pos,
		mode, chunk_size, &err);
	free(path);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);

	if (size - s > 0) {
		ss = chfs_pwrite(fd, buf + s, size - s, offset + s);
		if (ss < 0)
			ss = 0;
	}
	return (s + ss);
}

ssize_t
chfs_write(int fd, const void *buf, size_t size)
{
	struct chfs_fd_table *tab = get_fd_table(fd);
	ssize_t s;

	s = chfs_pwrite(fd, buf, size, tab->pos);
	if (s > 0)
		tab->pos += s;
	release_fd_table(tab);
	return (s);
}

ssize_t
chfs_pread(int fd, void *buf, size_t size, off_t offset)
{
	struct chfs_fd_table *tab = get_fd_table(fd);
	void *path;
	int index, local_pos, chunk_size, ret, err;
	size_t s = size, ss = 0, psize;

	if (tab == NULL)
		return (-1);

	chunk_size = tab->chunk_size;
	index = offset / chunk_size;
	local_pos = offset % chunk_size;

	path = path_index(tab->path, index, &psize);
	release_fd_table(tab);
	if (path == NULL)
		return (-1);
	ret = chfs_rpc_inode_read(path, psize, buf, &s, local_pos, &err);
	free(path);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);
	if (s <= 0)
		return (0);

	if (local_pos + s < chunk_size)
		return (s);
	if (size - s > 0) {
		ss = chfs_pread(fd, buf + s, size - s, offset + s);
		if (ss < 0)
			ss = 0;
	}
	return (s + ss);
}

ssize_t
chfs_read(int fd, void *buf, size_t size)
{
	struct chfs_fd_table *tab = get_fd_table(fd);
	ssize_t s;

	s = chfs_pread(fd, buf, size, tab->pos);
	if (s > 0)
		tab->pos += s;
	release_fd_table(tab);
	return (s);
}

int
chfs_unlink(const char *path)
{
	const char *p = skip_slash(path);
	int ret, err, i;
	size_t psize;
	void *pi;

	ret = chfs_rpc_remove((void *)p, strlen(p) + 1, &err);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);

	for (i = 1;; ++i) {
		pi = path_index(p, i, &psize);
		if (pi == NULL)
			break;
		ret = chfs_rpc_remove(pi, psize, &err);
		free(pi);
		if (ret != HG_SUCCESS || err != KV_SUCCESS)
			break;
	}
	return (0);
}

int
chfs_mkdir(const char *path, mode_t mode)
{
	const char *p = skip_slash(path);
	hg_return_t ret;
	int err;

	mode |= S_IFDIR;
	ret = chfs_rpc_inode_create((void *)p, strlen(p) + 1,
		mode, 0, &err);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);
	return (0);
}

int
chfs_rmdir(const char *path)
{
	const char *p = skip_slash(path);
	hg_return_t ret;
	int err;

	/* XXX check child entries */
	ret = chfs_rpc_remove((void *)p, strlen(p) + 1, &err);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);
	return (0);
}

static void
root_stat(struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFDIR | 0755;
}

int
chfs_stat(const char *path, struct stat *st)
{
	const char *p = skip_slash(path);
	struct fs_stat sb;
	size_t psize;
	void *pi;
	char *pp;
	hg_return_t ret;
	int err, i, p_len;

	if (p[0] == '\0') {
		root_stat(st);
		return (0);
	}
	p_len = strlen(p);
	if (p_len > 0 && p[p_len - 1] == '/') {
		pp = strdup(p);
		assert(pp);
		pp[p_len - 1] = '\0';
		ret = chfs_rpc_inode_stat(pp, p_len, &sb, &err);
		free(pp);
	} else
		ret = chfs_rpc_inode_stat((void *)p, p_len + 1, &sb, &err);
	if (ret != HG_SUCCESS || err != KV_SUCCESS)
		return (-1);
	st->st_mode = sb.mode;
	st->st_uid = sb.uid;
	st->st_gid = sb.gid;
	st->st_size = sb.size;
	st->st_mtim = sb.mtime;
	st->st_ctim = sb.ctime;
	st->st_nlink = 1;
	if (S_ISDIR(sb.mode) || sb.size < sb.chunk_size)
		return (0);

	for (i = 1;; ++i) {
		pi = path_index(p, i, &psize);
		if (pi == NULL)
			break;
		ret = chfs_rpc_inode_stat(pi, psize, &sb, &err);
		free(pi);
		if (ret != HG_SUCCESS || err != KV_SUCCESS)
			break;
		st->st_size += sb.size;
		if (sb.size == 0 || sb.size < sb.chunk_size)
			break;
	}
	return (0);
}

int
chfs_readdir(const char *path, void *buf,
	int (*filler)(void *, const char *, const struct stat *, off_t))
{
	const char *p = skip_slash(path);
	string_list_t node_list;
	hg_return_t ret;
	int err, i;

	ring_list_copy(&node_list);
	for (i = 0; i < node_list.n; ++i) {
		ret = fs_rpc_readdir(node_list.s[i], p, buf, filler, &err);
		if (ret != HG_SUCCESS || err != KV_SUCCESS)
			continue;
	}
	ring_list_copy_free(&node_list);
	return (0);
}