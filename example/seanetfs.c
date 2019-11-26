/*
  SSH file system
  Copyright (C) 2004  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#define _GNU_SOURCE /* avoid implicit declaration of *pt* functions */
#include "config.h"

#include <fuse.h>
#include <fuse_opt.h>
#if !defined(__CYGWIN__)
#  include <fuse_lowlevel.h>
#endif
#ifdef __APPLE__
#  include <fuse_darwin.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#ifndef __APPLE__
#  include <semaphore.h>
#endif
#include <pthread.h>
#include <netdb.h>
#include <signal.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <glib.h>
#include <pwd.h>
#include <grp.h>
#include <limits.h>
#ifdef __APPLE__
#  include <strings.h>
#  include <libgen.h>
#  include <darwin_compat.h>
#endif

#include "cache.h"

#ifndef MAP_LOCKED
#define MAP_LOCKED 0
#endif
#define NAME_MAX 255
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif


#define SSH_FXP_INIT                1
#define SSH_FXP_VERSION             2
#define SSH_FXP_OPEN                3
#define SSH_FXP_CLOSE               4
#define SSH_FXP_READ                5
#define SSH_FXP_WRITE               6
#define SSH_FXP_LSTAT               7
#define SSH_FXP_FSTAT               8
#define SSH_FXP_SETSTAT             9
#define SSH_FXP_FSETSTAT           10
#define SSH_FXP_OPENDIR            11
#define SSH_FXP_READDIR            12
#define SSH_FXP_REMOVE             13
#define SSH_FXP_MKDIR              14
#define SSH_FXP_RMDIR              15
#define SSH_FXP_REALPATH           16
#define SSH_FXP_STAT               17
#define SSH_FXP_RENAME             18
#define SSH_FXP_READLINK           19
#define SSH_FXP_SYMLINK            20
#define SSH_FXP_STATUS            101
#define SSH_FXP_HANDLE            102
#define SSH_FXP_DATA              103
#define SSH_FXP_NAME              104
#define SSH_FXP_ATTRS             105
#define SSH_FXP_EXTENDED          200
#define SSH_FXP_EXTENDED_REPLY    201

#define SSH_FILEXFER_ATTR_SIZE          0x00000001
#define SSH_FILEXFER_ATTR_UIDGID        0x00000002
#define SSH_FILEXFER_ATTR_PERMISSIONS   0x00000004
#define SSH_FILEXFER_ATTR_ACMODTIME     0x00000008
#define SSH_FILEXFER_ATTR_EXTENDED      0x80000000

#define SSH_FX_OK                            0
#define SSH_FX_EOF                           1
#define SSH_FX_NO_SUCH_FILE                  2
#define SSH_FX_PERMISSION_DENIED             3
#define SSH_FX_FAILURE                       4
#define SSH_FX_BAD_MESSAGE                   5
#define SSH_FX_NO_CONNECTION                 6
#define SSH_FX_CONNECTION_LOST               7
#define SSH_FX_OP_UNSUPPORTED                8

#define SSH_FXF_READ            0x00000001
#define SSH_FXF_WRITE           0x00000002
#define SSH_FXF_APPEND          0x00000004
#define SSH_FXF_CREAT           0x00000008
#define SSH_FXF_TRUNC           0x00000010
#define SSH_FXF_EXCL            0x00000020

/* statvfs@openssh.com f_flag flags */
#define SSH2_FXE_STATVFS_ST_RDONLY	0x00000001
#define SSH2_FXE_STATVFS_ST_NOSUID	0x00000002

#define SFTP_EXT_POSIX_RENAME "posix-rename@openssh.com"
#define SFTP_EXT_STATVFS "statvfs@openssh.com"
#define SFTP_EXT_HARDLINK "hardlink@openssh.com"
#define SFTP_EXT_FSYNC "fsync@openssh.com"

#define PROTO_VERSION 3

#define MY_EOF 1

#define MAX_REPLY_LEN (1 << 17)

#define RENAME_TEMP_CHARS 8

#define SFTP_SERVER_PATH "/usr/lib/sftp-server"

/* Asynchronous readdir parameters */
#define READDIR_START 2
#define READDIR_MAX 32
#define CHUNK_SIZE 128
#define MAX_PASSWORD 1024

#ifdef __APPLE__
static char seafile_program_path[PATH_MAX] = { 0 };
#endif /* __APPLE__ */

struct buffer {
	uint8_t *p;
	size_t len;
	size_t size;
};

struct list_head {
	struct list_head *prev;
	struct list_head *next;
};

struct request;
typedef void (*request_func)(struct request *);

struct request {
	unsigned int want_reply;
	sem_t ready;//这个是信号量变量，第一次见，用来多进程同步
	uint8_t reply_type;
	uint32_t id;
	int replied;
	int error;
	struct buffer reply;
	struct timeval start;
	void *data;
	request_func end_func;
	size_t len;
	struct list_head list;
};

struct seafile_io {
	int num_reqs;
	pthread_cond_t finished;
	int error;
};

struct read_req {
	struct seafile_io *sio;
	struct list_head list;
	struct buffer data;
	size_t size;
	ssize_t res; //sigend size_t
};

struct read_chunk {
	off_t offset;
	size_t size;
	int refs;
	long modifver;
	struct list_head reqs;
	struct seafile_io sio;
};
//实际上就是inode
struct seafile_inode {
	struct buffer handle;
	struct list_head write_reqs;
	pthread_cond_t write_finished;
	int write_error;
	struct read_chunk *readahead;
	struct context* ctx; 
	off_t next_pos;
	int is_seq;
	int connver;
	int modifver;
	int refs;
	char* root_manifest_eid;//rootmanifesteid很重要	
	uint32_t flags;
	uint64_t size = 0;//这个表示inode下面有多少个逻辑单元（文件/文件夹的个数，或者是
	//文件拆分的chunk的个数
	uint64_t bit_size = 0;//表示文件字节大小
	uint32_t uid = 0;//拥有者id
	uint32_t gid = 0;//拥有者所在gruop id
	uint32_t atime = 0;//文件上次打开时间
	uint32_t mtime = 0;//文件上次变动时间
	uint32_t ctime = 0;//inode 上次变动时间
	uint32_t mode = S_IFREG | 0777;//inode类型（表示是文件还是文件夹）
	uint32_t dev;//
	uint32_t n_link = 1;
};

struct seafile {
	char *directport;
	char *ssh_command;
	char *sftp_server;
	struct fuse_args ssh_args;
	char *workarounds;
	int rename_workaround;
	int renamexdev_workaround;
	int truncate_workaround;
	int buflimit_workaround;
	int unrel_append;
	int fstat_workaround;
	int createmode_workaround;
	int transform_symlinks;
	int follow_symlinks;
	int no_check_root;
	int detect_uid;
	int idmap;
	int nomap;
	int disable_hardlink;
	int dir_cache;
	int show_version;
	int show_help;
	int singlethread;
	char *mountpoint;
	char *uid_file;
	char *gid_file;
	GHashTable *uid_map;
	GHashTable *gid_map;
	GHashTable *r_uid_map;
	GHashTable *r_gid_map;
	unsigned max_read;
	unsigned max_write;
	unsigned ssh_ver;
	int sync_write;
	int sync_read;
	int sync_readdir;
  int direct_io;
	int debug;
	int verbose;
	int foreground;
	int reconnect;
	int delay_connect;
	int slave;
	char *host;
	char *base_path;
	GHashTable *reqtab;
	pthread_mutex_t lock;
	pthread_mutex_t lock_write;
	int processing_thread_started;
	unsigned int randseed;
	int rfd;
	int wfd;
	int ptyfd;
	int ptyslavefd;
	int connver;
	int server_version;
	unsigned remote_uid;
	unsigned local_uid;
	unsigned remote_gid;
	unsigned local_gid;
	int remote_uid_detected;
	unsigned blksize;
	char *progname;
	long modifver;
	unsigned outstanding_len;
	unsigned max_outstanding_len;
	pthread_cond_t outstanding_cond;
	int password_stdin;
	char *password;
	int ext_posix_rename;
	int ext_statvfs;
	int ext_hardlink;
	int ext_fsync;
	struct fuse_operations *op;

	/* statistics */
	uint64_t bytes_sent;
	uint64_t bytes_received;
	uint64_t num_sent;
	uint64_t num_received;
	unsigned int min_rtt;
	unsigned int max_rtt;
	uint64_t total_rtt;
	unsigned int num_connect;
};

static struct seafile seafile;

static const char *ssh_opts[] = {
	"AddressFamily",
	"BatchMode",
	"BindAddress",
	"BindInterface",
	"CertificateFile",
	"ChallengeResponseAuthentication",
	"CheckHostIP",
	"Cipher",
	"Ciphers",
	"Compression",
	"CompressionLevel",
	"ConnectionAttempts",
	"ConnectTimeout",
	"ControlMaster",
	"ControlPath",
	"ControlPersist",
	"FingerprintHash",
	"GlobalKnownHostsFile",
	"GSSAPIAuthentication",
	"GSSAPIDelegateCredentials",
	"HostbasedAuthentication",
	"HostbasedKeyTypes",
	"HostKeyAlgorithms",
	"HostKeyAlias",
	"HostName",
	"IdentitiesOnly",
	"IdentityFile",
	"IdentityAgent",
	"IPQoS",
	"KbdInteractiveAuthentication",
	"KbdInteractiveDevices",
	"KexAlgorithms",
	"LocalCommand",
	"LogLevel",
	"MACs",
	"NoHostAuthenticationForLocalhost",
	"NumberOfPasswordPrompts",
	"PasswordAuthentication",
	"PermitLocalCommand",
	"PKCS11Provider",
	"Port",
	"PreferredAuthentications",
	"ProxyCommand",
	"ProxyJump",
	"ProxyUseFdpass",
	"PubkeyAcceptedKeyTypes"
	"PubkeyAuthentication",
	"RekeyLimit",
	"RevokedHostKeys",
	"RhostsRSAAuthentication",
	"RSAAuthentication",
	"ServerAliveCountMax",
	"ServerAliveInterval",
	"SmartcardDevice",
	"StrictHostKeyChecking",
	"TCPKeepAlive",
	"UpdateHostKeys",
	"UsePrivilegedPort",
	"UserKnownHostsFile",
	"VerifyHostKeyDNS",
	"VisualHostKey",
	NULL,
};

enum {
	KEY_PORT,
	KEY_COMPRESS,
	KEY_CONFIGFILE,
};

enum {
	IDMAP_NONE,
	IDMAP_USER,
	IDMAP_FILE,
};

enum {
	NOMAP_IGNORE,
	NOMAP_ERROR,
};

#define seafile_OPT(t, p, v) { t, offsetof(struct seafile, p), v }

static struct fuse_opt seafile_opts[] = {
	seafile_OPT("directport=%s",     directport, 0),
	seafile_OPT("ssh_command=%s",    ssh_command, 0),
	seafile_OPT("sftp_server=%s",    sftp_server, 0),
	seafile_OPT("max_read=%u",       max_read, 0),
	seafile_OPT("max_write=%u",      max_write, 0),
	seafile_OPT("ssh_protocol=%u",   ssh_ver, 0),
	seafile_OPT("-1",                ssh_ver, 1),
	seafile_OPT("workaround=%s",     workarounds, 0),
	seafile_OPT("idmap=none",        idmap, IDMAP_NONE),
	seafile_OPT("idmap=user",        idmap, IDMAP_USER),
	seafile_OPT("idmap=file",        idmap, IDMAP_FILE),
	seafile_OPT("uidfile=%s",        uid_file, 0),
	seafile_OPT("gidfile=%s",        gid_file, 0),
	seafile_OPT("nomap=ignore",      nomap, NOMAP_IGNORE),
	seafile_OPT("nomap=error",       nomap, NOMAP_ERROR),
	seafile_OPT("seafile_sync",        sync_write, 1),
	seafile_OPT("no_readahead",      sync_read, 1),
	seafile_OPT("sync_readdir",      sync_readdir, 1),
	seafile_OPT("seafile_debug",       debug, 1),
	seafile_OPT("seafile_verbose",     verbose, 1),
	seafile_OPT("reconnect",         reconnect, 1),
	seafile_OPT("transform_symlinks", transform_symlinks, 1),
	seafile_OPT("follow_symlinks",   follow_symlinks, 1),
	seafile_OPT("no_check_root",     no_check_root, 1),
	seafile_OPT("password_stdin",    password_stdin, 1),
	seafile_OPT("delay_connect",     delay_connect, 1),
	seafile_OPT("slave",             slave, 1),
	seafile_OPT("disable_hardlink",  disable_hardlink, 1),
	seafile_OPT("dir_cache=yes", dir_cache, 1),
	seafile_OPT("dir_cache=no",  dir_cache, 0),
	seafile_OPT("direct_io",  direct_io, 1),

	seafile_OPT("-h",		show_help, 1),
	seafile_OPT("--help",	show_help, 1),
	seafile_OPT("-V",		show_version, 1),
	seafile_OPT("--version",	show_version, 1),
	seafile_OPT("-d",		debug, 1),
	seafile_OPT("debug",	debug, 1),
	seafile_OPT("-v",		verbose, 1),
	seafile_OPT("verbose",	verbose, 1),
	seafile_OPT("-f",		foreground, 1),
	seafile_OPT("-s",		singlethread, 1),

	FUSE_OPT_KEY("-p ",            KEY_PORT),
	FUSE_OPT_KEY("-C",             KEY_COMPRESS),
	FUSE_OPT_KEY("-F ",            KEY_CONFIGFILE),

	/* For backwards compatibility */
	seafile_OPT("cache=yes", dir_cache, 1),
	seafile_OPT("cache=no",  dir_cache, 0),
	
	FUSE_OPT_KEY("writeback_cache=no", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("unreliable_append", FUSE_OPT_KEY_DISCARD),

	/* These may come in from /etc/fstab - we just ignore them */
	FUSE_OPT_KEY("auto", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("noauto", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("user", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("nouser", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("users", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("_netdev", FUSE_OPT_KEY_DISCARD),

	FUSE_OPT_END
};

static struct fuse_opt workaround_opts[] = {
	seafile_OPT("none",       rename_workaround, 0),
	seafile_OPT("none",       truncate_workaround, 0),
	seafile_OPT("none",       buflimit_workaround, 0),
	seafile_OPT("none",       fstat_workaround, 0),
	seafile_OPT("rename",     rename_workaround, 1),
	seafile_OPT("norename",   rename_workaround, 0),
	seafile_OPT("renamexdev",   renamexdev_workaround, 1),
	seafile_OPT("norenamexdev", renamexdev_workaround, 0),
	seafile_OPT("truncate",   truncate_workaround, 1),
	seafile_OPT("notruncate", truncate_workaround, 0),
	seafile_OPT("buflimit",   buflimit_workaround, 1),
	seafile_OPT("nobuflimit", buflimit_workaround, 0),
	seafile_OPT("fstat",      fstat_workaround, 1),
	seafile_OPT("nofstat",    fstat_workaround, 0),
	seafile_OPT("createmode",   createmode_workaround, 1),
	seafile_OPT("nocreatemode", createmode_workaround, 0),
	FUSE_OPT_END
};

#define DEBUG(format, args...)						\
	do { if (seafile.debug) fprintf(stderr, format, args); } while(0)

static const char *type_name(uint8_t type)
{
	switch(type) {
	case SSH_FXP_INIT:           return "INIT";
	case SSH_FXP_VERSION:        return "VERSION";
	case SSH_FXP_OPEN:           return "OPEN";
	case SSH_FXP_CLOSE:          return "CLOSE";
	case SSH_FXP_READ:           return "READ";
	case SSH_FXP_WRITE:          return "WRITE";
	case SSH_FXP_LSTAT:          return "LSTAT";
	case SSH_FXP_FSTAT:          return "FSTAT";
	case SSH_FXP_SETSTAT:        return "SETSTAT";
	case SSH_FXP_FSETSTAT:       return "FSETSTAT";
	case SSH_FXP_OPENDIR:        return "OPENDIR";
	case SSH_FXP_READDIR:        return "READDIR";
	case SSH_FXP_REMOVE:         return "REMOVE";
	case SSH_FXP_MKDIR:          return "MKDIR";
	case SSH_FXP_RMDIR:          return "RMDIR";
	case SSH_FXP_REALPATH:       return "REALPATH";
	case SSH_FXP_STAT:           return "STAT";
	case SSH_FXP_RENAME:         return "RENAME";
	case SSH_FXP_READLINK:       return "READLINK";
	case SSH_FXP_SYMLINK:        return "SYMLINK";
	case SSH_FXP_STATUS:         return "STATUS";
	case SSH_FXP_HANDLE:         return "HANDLE";
	case SSH_FXP_DATA:           return "DATA";
	case SSH_FXP_NAME:           return "NAME";
	case SSH_FXP_ATTRS:          return "ATTRS";
	case SSH_FXP_EXTENDED:       return "EXTENDED";
	case SSH_FXP_EXTENDED_REPLY: return "EXTENDED_REPLY";
	default:                     return "???";
	}
}
// struct read_req {
// 	struct seafile_io *sio;
// 	struct list_head list;
// 	struct buffer data;
// 	size_t size;
// 	ssize_t res; //sigend size_t
// };

// struct read_chunk {
// 	off_t offset;
// 	size_t size;
// 	int refs;
// 	long modifver;
// 	struct list_head reqs;
// 	struct seafile_io sio;
// };
#define container_of(ptr, type, member) ({				\
			const typeof( ((type *)0)->member ) *__mptr = (ptr); \
			(type *)( (char *)__mptr - offsetof(type,member) );})

#define list_entry(ptr, type, member)		\
	container_of(ptr, type, member)

static void list_init(struct list_head *head)
{
	head->next = head;
	head->prev = head;
}

static void list_add(struct list_head *new, struct list_head *head)
{
	struct list_head *prev = head;
	struct list_head *next = head->next;
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static void list_del(struct list_head *entry)
{
	struct list_head *prev = entry->prev;
	struct list_head *next = entry->next;
	next->prev = prev;
	prev->next = next;

}

static int list_empty(const struct list_head *head)
{
	return head->next == head;
}

/* given a pointer to the uid/gid, and the mapping table, remap the
 * uid/gid, if necessary */
static inline int translate_id(uint32_t *id, GHashTable *map)
{
	gpointer id_p;
	if (g_hash_table_lookup_extended(map, GUINT_TO_POINTER(*id), NULL, &id_p)) {
		*id = GPOINTER_TO_UINT(id_p);
		return 0;
	}
	switch (seafile.nomap) {
	case NOMAP_ERROR: return -1;
	case NOMAP_IGNORE: return 0;
	default:
		fprintf(stderr, "internal error\n");
		abort();
	}
}

static inline void buf_init(struct buffer *buf, size_t size)
{
	if (size) {
		buf->p = (uint8_t *) malloc(size);
		if (!buf->p) {
			fprintf(stderr, "seafile: memory allocation failed\n");
			abort();
		}
	} else
		buf->p = NULL;
	buf->len = 0;
	buf->size = size;
}

static inline void buf_free(struct buffer *buf)
{
	free(buf->p);
}

static inline void buf_finish(struct buffer *buf)
{
	buf->len = buf->size;
}

static inline void buf_clear(struct buffer *buf)
{
	buf_free(buf);
	buf_init(buf, 0);
}

static void buf_resize(struct buffer *buf, size_t len)
{
	buf->size = (buf->len + len + 63) & ~31;
	buf->p = (uint8_t *) realloc(buf->p, buf->size);
	if (!buf->p) {
		fprintf(stderr, "seafile: memory allocation failed\n");
		abort();
	}
}

static inline void buf_check_add(struct buffer *buf, size_t len)
{
	if (buf->len + len > buf->size)
		buf_resize(buf, len);
}

#define _buf_add_mem(b, d, l)			\
	buf_check_add(b, l);			\
	memcpy(b->p + b->len, d, l);		\
	b->len += l;


static inline void buf_add_mem(struct buffer *buf, const void *data,
                               size_t len)
{
	_buf_add_mem(buf, data, len);
}

static inline void buf_add_buf(struct buffer *buf, const struct buffer *bufa)
{
	_buf_add_mem(buf, bufa->p, bufa->len);
}

static inline void buf_add_uint8(struct buffer *buf, uint8_t val)
{
	_buf_add_mem(buf, &val, 1);
}

static inline void buf_add_uint32(struct buffer *buf, uint32_t val)
{
	uint32_t nval = htonl(val);
	_buf_add_mem(buf, &nval, 4);
}

static inline void buf_add_uint64(struct buffer *buf, uint64_t val)
{
	buf_add_uint32(buf, val >> 32);
	buf_add_uint32(buf, val & 0xffffffff);
}

static inline void buf_add_data(struct buffer *buf, const struct buffer *data)
{
	buf_add_uint32(buf, data->len);
	buf_add_mem(buf, data->p, data->len);
}

static inline void buf_add_string(struct buffer *buf, const char *str)
{
	struct buffer data;
	data.p = (uint8_t *) str;
	data.len = strlen(str);
	buf_add_data(buf, &data);
}

static inline void buf_add_path(struct buffer *buf, const char *path)
{
	char *realpath;

	if (seafile.base_path[0]) {
		if (path[1]) {
			if (seafile.base_path[strlen(seafile.base_path)-1] != '/') {
				realpath = g_strdup_printf("%s/%s",
							   seafile.base_path,
							   path + 1);
			} else {
				realpath = g_strdup_printf("%s%s",
							   seafile.base_path,
							   path + 1);
			}
		} else {
			realpath = g_strdup(seafile.base_path);
		}
	} else {
		if (path[1])
			realpath = g_strdup(path + 1);
		else
			realpath = g_strdup(".");
	}
	buf_add_string(buf, realpath);
	g_free(realpath);
}

static int buf_check_get(struct buffer *buf, size_t len)
{
	if (buf->len + len > buf->size) {
		fprintf(stderr, "buffer too short\n");
		return -1;
	} else
		return 0;
}

static inline int buf_get_mem(struct buffer *buf, void *data, size_t len)
{
	if (buf_check_get(buf, len) == -1)
		return -1;
	memcpy(data, buf->p + buf->len, len);
	buf->len += len;
	return 0;
}

static inline int buf_get_uint8(struct buffer *buf, uint8_t *val)
{
	return buf_get_mem(buf, val, 1);
}

static inline int buf_get_uint32(struct buffer *buf, uint32_t *val)
{
	uint32_t nval;
	if (buf_get_mem(buf, &nval, 4) == -1)
		return -1;
	*val = ntohl(nval);
	return 0;
}

static inline int buf_get_uint64(struct buffer *buf, uint64_t *val)
{
	uint32_t val1;
	uint32_t val2;
	if (buf_get_uint32(buf, &val1) == -1 ||
	    buf_get_uint32(buf, &val2) == -1) {
		return -1;
	}
	*val = ((uint64_t) val1 << 32) + val2;
	return 0;
}

static inline int buf_get_data(struct buffer *buf, struct buffer *data)
{
	uint32_t len;
	if (buf_get_uint32(buf, &len) == -1 || len > buf->size - buf->len)

		return -1;
	buf_init(data, len + 1);
	data->size = len;
	if (buf_get_mem(buf, data->p, data->size) == -1) {
		buf_free(data);
		return -1;
	}
	return 0;
}

static inline int buf_get_string(struct buffer *buf, char **str) 
//为什么用二级指针？
{
	struct buffer data;
	if (buf_get_data(buf, &data) == -1)
		return -1;
	data.p[data.size] = '\0';
	*str = (char *) data.p;
	return 0;
}
//这个函数可以大体看出如何根据远程信息修改本地inode。
//为什么没有inode号
static int buf_get_attrs(struct seafile_inode *sf, struct stat *stbuf, int *flagsp)
{

	memset(stbuf, 0, sizeof(struct stat));
	stbuf->st_mode = sf->mode;
	stbuf->st_nlink = 1;
	stbuf->st_size = sf->size;
	if (seafile.blksize) {
		stbuf->st_blksize = seafile.blksize;
		stbuf->st_blocks = ((size + seafile.blksize - 1) &
			~((unsigned long long) seafile.blksize - 1)) >> 9;
	}
	stbuf->st_uid = sf->uid;
	stbuf->st_gid = sf->gid;
	stbuf->st_atime = sf->atime;
	stbuf->st_ctime = stbuf->st_mtime = sf->mtime;
	return 0;
}
//从buffer里面获取文件系统的信息
static int buf_get_statvfs(struct buffer *buf, struct statvfs *stbuf)
{
// 	struct statfs 
// { 
//    long    f_type;     /* 文件系统类型  */ 
//    long    f_bsize;    /* 经过优化的传输块大小  */ 
//    long    f_blocks;   /* 文件系统数据块总数 */ 
//    long    f_bfree;    /* 可用块数 */ 
//    long    f_bavail;   /* 非超级用户可获取的块数 */ 
//    long    f_files;    /* 文件结点总数 */ 
//    long    f_ffree;    /* 可用文件结点数 */ 
//    fsid_t  f_fsid;     /* 文件系统标识 */ 
//    long    f_namelen;  /* 文件名的最大长度 */ 
// }; 
// ***************************************************************
	uint64_t bsize;
	uint64_t frsize;
	uint64_t blocks;
	uint64_t bfree;
	uint64_t bavail;
	uint64_t files;
	uint64_t ffree;
	uint64_t favail;
	uint64_t fsid;
	uint64_t flag;
	uint64_t namemax;

	if (buf_get_uint64(buf, &bsize) == -1 ||
	    buf_get_uint64(buf, &frsize) == -1 ||
	    buf_get_uint64(buf, &blocks) == -1 ||
	    buf_get_uint64(buf, &bfree) == -1 ||
	    buf_get_uint64(buf, &bavail) == -1 ||
	    buf_get_uint64(buf, &files) == -1 ||
	    buf_get_uint64(buf, &ffree) == -1 ||
	    buf_get_uint64(buf, &favail) == -1 ||
	    buf_get_uint64(buf, &fsid) == -1 ||
	    buf_get_uint64(buf, &flag) == -1 ||
	    buf_get_uint64(buf, &namemax) == -1) {
		return -1;
	}

	memset(stbuf, 0, sizeof(struct statvfs));
	stbuf->f_bsize = bsize;
	stbuf->f_frsize = frsize;
	stbuf->f_blocks = blocks;
	stbuf->f_bfree = bfree;
	stbuf->f_bavail = bavail;
	stbuf->f_files = files;
	stbuf->f_ffree = ffree;
	stbuf->f_favail = favail;
	stbuf->f_namemax = namemax;

	return 0;
}

// typedef int(* fuse_fill_dir_t) 
// (void *buf, const char *name, const struct stat *stbuf, off_t off,
// 	enum fuse_fill_dir_flags flags)

// Function to add an entry in a readdir() operation

// The off parameter can be any non-zero value that enables the 
// filesystem to identify the current point in the directory stream. 
// It does not need to be the actual physical position. 
// A value of zero is reserved to indicate that seeking in directories 
// is not supported.

// Parameters
// buf	the buffer passed to the readdir() operation
// name	the file name of the directory entry
// stat	file attributes, can be NULL
// off	offset of the next entry or zero
// flags	fill flags
// Returns
// 1 if buffer is full, zero otherwise
static int buf_get_entries(struct buffer *buf, void *dbuf,
                           fuse_fill_dir_t filler)
{
	uint32_t count;
	unsigned i;

	if (buf_get_uint32(buf, &count) == -1)
		return -EIO;

	for (i = 0; i < count; i++) {
		int err = -1;
		char *name;
		char *longname;
		struct stat stbuf;
		if (buf_get_string(buf, &name) == -1)
			return -EIO;
		if (buf_get_string(buf, &longname) != -1) {
			free(longname);//为什么要立刻free掉？
			err = buf_get_attrs(buf, &stbuf, NULL);
			if (!err) {
				if (seafile.follow_symlinks &&
				    S_ISLNK(stbuf.st_mode)) {
					stbuf.st_mode = 0;
				}
				filler(dbuf, name, &stbuf, 0, 0);
			}
		}
		free(name);
		if (err)
			return err;
	}
	return 0;
}

static void ssh_add_arg(const char *arg)
{
	if (fuse_opt_add_arg(&seafile.ssh_args, arg) == -1)
		_exit(1);
}
// 相关函数：readdir, write, fcntl, close, lseek, readlink, fread

// 头文件：#include <unistd.h>

// 定义函数：ssize_t read(int fd, void * buf, size_t count);

// 函数说明：read()会把参数fd 所指的文件传送count 个字节到buf 指针所指的内存中. 
// 若参数count 为0, 则read()不会有作用并返回0. 返回值为实际读取到的字节数,
//  如果返回0, 表示已到达文件尾或是无可读取的数据,此外文件读写位置会随读取到的字节移动.

static int pty_expect_loop(void)
{
	int res;
	char buf[256];
	const char *passwd_str = "assword:";
	int timeout = 60 * 1000; /* 1min timeout for the prompt to appear */
	int passwd_len = strlen(passwd_str);
	int len = 0;
	char c;

	while (1) {
		struct pollfd fds[2];

		fds[0].fd = seafile.rfd;
		fds[0].events = POLLIN;
		fds[1].fd = seafile.ptyfd;
		fds[1].events = POLLIN;
		res = poll(fds, 2, timeout);
		if (res == -1) {
			perror("poll");
			return -1;
		}
		if (res == 0) {
			fprintf(stderr, "Timeout waiting for prompt\n");
			return -1;
		}
		if (fds[0].revents) {
			/*
			 * Something happened on stdout of ssh, this
			 * either means, that we are connected, or
			 * that we are disconnected.  In any case the
			 * password doesn't matter any more.
			 */
			break;
		}

		res = read(seafile.ptyfd, &c, 1);
		if (res == -1) {
			perror("read");
			return -1;
		}
		if (res == 0) {
			fprintf(stderr, "EOF while waiting for prompt\n");
			return -1;
		}
		buf[len] = c;
		len++;
		if (len == passwd_len) {
			if (memcmp(buf, passwd_str, passwd_len) == 0) {
				write(seafile.ptyfd, seafile.password,
				      strlen(seafile.password));
			}
			memmove(buf, buf + 1, passwd_len - 1);
			len--;
		}
	}

	if (!seafile.reconnect) {
		size_t size = getpagesize();

		memset(seafile.password, 0, size);
		munmap(seafile.password, size);
		seafile.password = NULL;
	}

	return 0;
}

static int pty_master(char **name)
{
	int mfd;

	mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (mfd == -1) {
		perror("failed to open pty");
		return -1;
	}
	if (grantpt(mfd) != 0) {
		perror("grantpt");
		return -1;
	}
	if (unlockpt(mfd) != 0) {
		perror("unlockpt");
		return -1;
	}
	*name = ptsname(mfd);

	return mfd;
}

static void replace_arg(char **argp, const char *newarg)
{
	free(*argp);
	*argp = strdup(newarg);
// 	功 能: 将串拷贝到新建的位置处
// strdup()在内部调用了malloc()为变量分配内存，不需要使用返回的字符串时，
// 需要用free()释放相应的内存空间，否则会造成内存泄漏。
	if (*argp == NULL) {
		fprintf(stderr, "seafile: memory allocation failed\n");
		abort();
	}
}

static int start_ssh(void)
{
	char *ptyname = NULL;
	int sockpair[2];
	int pid;

	if (seafile.password_stdin) {

		seafile.ptyfd = pty_master(&ptyname);
		if (seafile.ptyfd == -1)
			return -1;

		seafile.ptyslavefd = open(ptyname, O_RDWR | O_NOCTTY);
		if (seafile.ptyslavefd == -1)
			return -1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockpair) == -1) {
		perror("failed to create socket pair");
		return -1;
	}
	seafile.rfd = sockpair[0];
	seafile.wfd = sockpair[0];

	pid = fork();
// 	pid_t fork( void);
// （pid_t 是一个宏定义，其实质是int 被定义在#include<sys/types.h>中）
// 返回值： 若成功调用一次则返回两个值，子进程返回0，父进程返回子进程ID；否则，出错返回-1
	if (pid == -1) {
		perror("failed to fork");
		close(sockpair[1]);
		return -1;
	} else if (pid == 0) {
		int devnull;

		devnull = open("/dev/null", O_WRONLY);

		if (dup2(sockpair[1], 0) == -1 || dup2(sockpair[1], 1) == -1) {
			perror("failed to redirect input/output");
			_exit(1);
		}
		if (!seafile.verbose && !seafile.foreground && devnull != -1)
			dup2(devnull, 2);

		close(devnull);
		close(sockpair[0]);
		close(sockpair[1]);

		switch (fork()) {
		case -1:
			perror("failed to fork");
			_exit(1);
		case 0:
			break;
		default:
			_exit(0);
		}
		chdir("/");//更换当前工作目录

		if (seafile.password_stdin) {
			int sfd;

			setsid();
			sfd = open(ptyname, O_RDWR);
			if (sfd == -1) {
				perror(ptyname);
				_exit(1);
			}
			close(sfd);
			close(seafile.ptyslavefd);
			close(seafile.ptyfd);
		}

		if (seafile.debug) {
			int i;

			fprintf(stderr, "executing");
			for (i = 0; i < seafile.ssh_args.argc; i++)
				fprintf(stderr, " <%s>",
					seafile.ssh_args.argv[i]);
			fprintf(stderr, "\n");
		}

		execvp(seafile.ssh_args.argv[0], seafile.ssh_args.argv);
		fprintf(stderr, "failed to execute '%s': %s\n",
			seafile.ssh_args.argv[0], strerror(errno));
		_exit(1);
	}
	waitpid(pid, NULL, 0);
	close(sockpair[1]);
	return 0;
}

static int connect_slave()
{
	seafile.rfd = STDIN_FILENO;
	seafile.wfd = STDOUT_FILENO;
	return 0;
}

static int connect_to(char *host, char *port)
{
	int err;
	int sock;
	int opt;
	struct addrinfo *ai;
	struct addrinfo hint;

	memset(&hint, 0, sizeof(hint));
	hint.ai_family = PF_INET;
	hint.ai_socktype = SOCK_STREAM;
	err = getaddrinfo(host, port, &hint, &ai);
	if (err) {
		fprintf(stderr, "failed to resolve %s:%s: %s\n", host, port,
			gai_strerror(err));
		return -1;
	}
	sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (sock == -1) {
		perror("failed to create socket");
		freeaddrinfo(ai);
		return -1;
	}
	err = connect(sock, ai->ai_addr, ai->ai_addrlen);
	if (err == -1) {
		perror("failed to connect");
		freeaddrinfo(ai);
		close(sock);
		return -1;
	}
	opt = 1;
	err = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	if (err == -1)
		perror("warning: failed to set TCP_NODELAY");

	freeaddrinfo(ai);

	seafile.rfd = sock;
	seafile.wfd = sock;
	return 0;
}

// struct iovec {
// 	 /* Starting address (内存起始地址）*/
//    void  *iov_base;   
   
//     /* Number of bytes to transfer（这块内存长度） */
//    size_t iov_len;    
//        };

static int do_write(struct iovec *iov, size_t count)
{
	int res;
	while (count) {
		res = writev(seafile.wfd, iov, count);
		// iovcnt不应超过IOV_MAX=1024，但是实际会大于这个值，所以需要多次调用writev。
		if (res == -1) {
			perror("write");
			return -1;
		} else if (res == 0) {
			fprintf(stderr, "zero write\n");
			return -1;
		}
		do {
			if ((unsigned) res < iov->iov_len) {
				iov->iov_len -= res;
				iov->iov_base += res;
				break;
			} else {
				res -= iov->iov_len;
				count --;
				iov ++;
			}
		} while(count);
	}
	return 0;
}

static uint32_t sftp_get_id(void)
{
	static uint32_t idctr;
	return idctr++;
}
// struct buffer {
// 	uint8_t *p;
// 	size_t len;
// 	size_t size; 代表总大小
// };
static void buf_to_iov(const struct buffer *buf, struct iovec *iov)
{
	iov->iov_base = buf->p;
	iov->iov_len = buf->len;
}
//计算iov列表中离散存储空间的总长度
static size_t iov_length(const struct iovec *iov, unsigned long nr_segs)
{
	unsigned long seg;
	size_t ret = 0;

	for (seg = 0; seg < nr_segs; seg++)
		ret += iov[seg].iov_len;
	return ret;
}

#define SFTP_MAX_IOV 3

static int sftp_send_iov(uint8_t type, uint32_t id, struct iovec iov[],
                         size_t count)
{
	int res;
	struct buffer buf;
	struct iovec iovout[SFTP_MAX_IOV];
	unsigned i;
	unsigned nout = 0;

	assert(count <= SFTP_MAX_IOV - 1);
	buf_init(&buf, 9);
	buf_add_uint32(&buf, iov_length(iov, count) + 5);
	buf_add_uint8(&buf, type);
	buf_add_uint32(&buf, id);
	buf_to_iov(&buf, &iovout[nout++]);
	for (i = 0; i < count; i++)
		iovout[nout++] = iov[i];
	pthread_mutex_lock(&seafile.lock_write);
	res = do_write(iovout, nout);
	pthread_mutex_unlock(&seafile.lock_write);
	buf_free(&buf);
	return res;
}

static int do_read(struct buffer *buf)
{
	int res;
	uint8_t *p = buf->p;
	size_t size = buf->size;
	while (size) {
		res = read(seafile.rfd, p, size);
		if (res == -1) {
			perror("read");
			return -1;
		} else if (res == 0) {
			fprintf(stderr, "remote host has disconnected\n");
			return -1;
		}
		size -= res;
		p += res;
	}
	return 0;
}

static int sftp_read(uint8_t *type, struct buffer *buf)
{
	int res;
	struct buffer buf2;
	uint32_t len;
	buf_init(&buf2, 5);
	res = do_read(&buf2);
	if (res != -1) {
		if (buf_get_uint32(&buf2, &len) == -1)
			return -1;
		if (len > MAX_REPLY_LEN) {
			fprintf(stderr, "reply len too large: %u\n", len);
			return -1;
		}
		if (buf_get_uint8(&buf2, type) == -1)
			return -1;
		buf_init(buf, len - 1);
		res = do_read(buf);
	}
	buf_free(&buf2);
	return res;
}

static void request_free(struct request *req)
{
	buf_free(&req->reply);
	sem_destroy(&req->ready);
	g_free(req);
}


static void chunk_free(struct read_chunk *chunk)
{
	while (!list_empty(&chunk->reqs)) {
		struct read_req *rreq;

		rreq = list_entry(chunk->reqs.prev, struct read_req, list);
		list_del(&rreq->list);
		buf_free(&rreq->data);
		g_free(rreq);
	}
	g_free(chunk);
}

static void chunk_put(struct read_chunk *chunk)
{
	if (chunk) {
		chunk->refs--;
		if (!chunk->refs)
			chunk_free(chunk);
	}
}

static void chunk_put_locked(struct read_chunk *chunk)
{
	pthread_mutex_lock(&seafile.lock);
	chunk_put(chunk);
	pthread_mutex_unlock(&seafile.lock);
}

static int clean_req(void *key_, struct request *req, gpointer user_data_)
{
	(void) key_;
	(void) user_data_;

	req->error = -EIO;
	if (req->want_reply)
		sem_post(&req->ready);
	else {
		if (req->end_func)
			req->end_func(req);
		request_free(req);
	}
	return TRUE;
}
// struct request {
// 	unsigned int want_reply;
// 	sem_t ready;//这个是信号量变量，第一次见，用来多进程同步
// 	uint8_t reply_type;
// 	uint32_t id;
// 	int replied;
// 	int error;
// 	struct buffer reply;
// 	struct timeval start;
// 	void *data;
// 	request_func end_func;
// 	size_t len;
// 	struct list_head list;
// };

static int process_one_request(void)
{
	int res;
	struct buffer buf;
	uint8_t type;
	struct request *req;
	uint32_t id;//这个id应该是request hash table 里面的key，可以用来索引struct request

	buf_init(&buf, 0);
	res = sftp_read(&type, &buf);
	if (res == -1)
		return -1;
	if (buf_get_uint32(&buf, &id) == -1)
		return -1;

// g_hash_table_lookup (GHashTable *hash_table,
//                      gconstpointer key);
// Looks up a key in a GHashTable. Note that this function cannot
//  distinguish between a key that is not present and one which is
//   present and has the value NULL. 
// If you need this distinction, use g_hash_table_lookup_extended().



// 	#define GPOINTER_TO_UINT(p) ((guint) (gulong) (p))
// Extracts an unsigned integer from a pointer. The integer must have 
// been stored in the pointer with GUINT_TO_POINTER().

// Parameters
// p pointer to extract an unsigned integer from


	pthread_mutex_lock(&seafile.lock);
	req = (struct request *)
		g_hash_table_lookup(seafile.reqtab, GUINT_TO_POINTER(id));
	if (req == NULL)
		fprintf(stderr, "request %i not found\n", id);
	else {
		int was_over;

		was_over = seafile.outstanding_len > seafile.max_outstanding_len;
		seafile.outstanding_len -= req->len;
		if (was_over &&
		    seafile.outstanding_len <= seafile.max_outstanding_len) {
			pthread_cond_broadcast(&seafile.outstanding_cond);
		}
		g_hash_table_remove(seafile.reqtab, GUINT_TO_POINTER(id));
	}
	pthread_mutex_unlock(&seafile.lock);
	if (req != NULL) {
		if (seafile.debug) {
			struct timeval now;
			unsigned int difftime;
			unsigned msgsize = buf.size + 5;

			gettimeofday(&now, NULL);
			difftime = (now.tv_sec - req->start.tv_sec) * 1000;
			difftime += (now.tv_usec - req->start.tv_usec) / 1000;
			DEBUG("  [%05i] %14s %8ubytes (%ims)\n", id,
			      type_name(type), msgsize, difftime);

			if (difftime < seafile.min_rtt || !seafile.num_received)
				seafile.min_rtt = difftime;
			if (difftime > seafile.max_rtt)
				seafile.max_rtt = difftime;
			seafile.total_rtt += difftime;
			seafile.num_received++;
			seafile.bytes_received += msgsize;
		}
		req->reply = buf;
		req->reply_type = type;
		req->replied = 1;
		if (req->want_reply)
			sem_post(&req->ready);
		else {
			if (req->end_func) {
				pthread_mutex_lock(&seafile.lock);
				req->end_func(req);
				pthread_mutex_unlock(&seafile.lock);
			}
			request_free(req);
		}
	} else
		buf_free(&buf);

	return 0;
}

static void close_conn(void)
{
	close(seafile.rfd);
	if (seafile.rfd != seafile.wfd)
		close(seafile.wfd);
	seafile.rfd = -1;
	seafile.wfd = -1;
	if (seafile.ptyfd != -1) {
		close(seafile.ptyfd);
		seafile.ptyfd = -1;
	}
	if (seafile.ptyslavefd != -1) {
		close(seafile.ptyslavefd);
		seafile.ptyslavefd = -1;
	}
}

static void *process_requests(void *data_)
{
	(void) data_;

	while (1) {
		if (process_one_request() == -1)
			break;
	}

	pthread_mutex_lock(&seafile.lock);
	seafile.processing_thread_started = 0;
	close_conn();
	g_hash_table_foreach_remove(seafile.reqtab, (GHRFunc) clean_req, NULL);
	seafile.connver ++;
	seafile.outstanding_len = 0;
	pthread_cond_broadcast(&seafile.outstanding_cond);
	pthread_mutex_unlock(&seafile.lock);

	if (!seafile.reconnect) {
		/* harakiri */
		kill(getpid(), SIGTERM);
	}
	return NULL;
}

static int sftp_init_reply_ok(struct buffer *buf, uint32_t *version)
{
	uint32_t len;
	uint8_t type;

	if (buf_get_uint32(buf, &len) == -1)
		return -1;

	if (len < 5 || len > MAX_REPLY_LEN)
		return 1;

	if (buf_get_uint8(buf, &type) == -1)
		return -1;

	if (type != SSH_FXP_VERSION)
		return 1;

	if (buf_get_uint32(buf, version) == -1)
		return -1;

	DEBUG("Server version: %u\n", *version);

	if (len > 5) {
		struct buffer buf2;

		buf_init(&buf2, len - 5);
		if (do_read(&buf2) == -1) {
			buf_free(&buf2);
			return -1;
		}

		do {
			char *ext;
			char *extdata;

			if (buf_get_string(&buf2, &ext) == -1 ||
			    buf_get_string(&buf2, &extdata) == -1) {
				buf_free(&buf2);
				return -1;
			}

			DEBUG("Extension: %s <%s>\n", ext, extdata);

			if (strcmp(ext, SFTP_EXT_POSIX_RENAME) == 0 &&
			    strcmp(extdata, "1") == 0) {
				seafile.ext_posix_rename = 1;
				seafile.rename_workaround = 0;
			}
			if (strcmp(ext, SFTP_EXT_STATVFS) == 0 &&
			    strcmp(extdata, "2") == 0)
				seafile.ext_statvfs = 1;
			if (strcmp(ext, SFTP_EXT_HARDLINK) == 0 &&
			    strcmp(extdata, "1") == 0)
				seafile.ext_hardlink = 1;
			if (strcmp(ext, SFTP_EXT_FSYNC) == 0 &&
			    strcmp(extdata, "1") == 0)
				seafile.ext_fsync = 1;
		} while (buf2.len < buf2.size);
		buf_free(&buf2);
	}
	return 0;
}

static int sftp_find_init_reply(uint32_t *version)
{
	int res;
	struct buffer buf;

	buf_init(&buf, 9);
	res = do_read(&buf);
	while (res != -1) {
		struct buffer buf2;

		res = sftp_init_reply_ok(&buf, version);
		if (res <= 0)
			break;

		/* Iterate over any rubbish until the version reply is found */
		DEBUG("%c", *buf.p);
		memmove(buf.p, buf.p + 1, buf.size - 1);
		buf.len = 0;
		buf2.p = buf.p + buf.size - 1;
		buf2.size = 1;
		res = do_read(&buf2);
	}
	buf_free(&buf);
	return res;
}

static int sftp_init()
{
	int res = -1;
	uint32_t version = 0;
	struct buffer buf;
	buf_init(&buf, 0);
	if (sftp_send_iov(SSH_FXP_INIT, PROTO_VERSION, NULL, 0) == -1)
		goto out;

	if (seafile.password_stdin && pty_expect_loop() == -1)
		goto out;

	if (sftp_find_init_reply(&version) == -1)
		goto out;

	seafile.server_version = version;
	if (version > PROTO_VERSION) {
		fprintf(stderr,
			"Warning: server uses version: %i, we support: %i\n",
			version, PROTO_VERSION);
	}
	res = 0;

out:
	buf_free(&buf);
	return res;
}

static int sftp_error_to_errno(uint32_t error)
{
	switch (error) {
	case SSH_FX_OK:                return 0;
	case SSH_FX_NO_SUCH_FILE:      return ENOENT;
	case SSH_FX_PERMISSION_DENIED: return EACCES;
	case SSH_FX_FAILURE:           return EPERM;
	case SSH_FX_BAD_MESSAGE:       return EBADMSG;
	case SSH_FX_NO_CONNECTION:     return ENOTCONN;
	case SSH_FX_CONNECTION_LOST:   return ECONNABORTED;
	case SSH_FX_OP_UNSUPPORTED:    return EOPNOTSUPP;
	default:                       return EIO;
	}
}

static void sftp_detect_uid()
{
	int flags;
	uint32_t id = sftp_get_id();
	uint32_t replid;
	uint8_t type;
	struct buffer buf;
	struct stat stbuf;
	struct iovec iov[1];

	buf_init(&buf, 5);
	buf_add_string(&buf, ".");
	buf_to_iov(&buf, &iov[0]);
	if (sftp_send_iov(SSH_FXP_STAT, id, iov, 1) == -1)
		goto out;
	buf_clear(&buf);
	if (sftp_read(&type, &buf) == -1)
		goto out;
	if (type != SSH_FXP_ATTRS && type != SSH_FXP_STATUS) {
		fprintf(stderr, "protocol error\n");
		goto out;
	}
	if (buf_get_uint32(&buf, &replid) == -1)
		goto out;
	if (replid != id) {
		fprintf(stderr, "bad reply ID\n");
		goto out;
	}
	if (type == SSH_FXP_STATUS) {
		uint32_t serr;
		if (buf_get_uint32(&buf, &serr) == -1)
			goto out;

		fprintf(stderr, "failed to stat home directory (%i)\n", serr);
		goto out;
	}
	if (buf_get_attrs(&buf, &stbuf, &flags) != 0)
		goto out;

	if (!(flags & SSH_FILEXFER_ATTR_UIDGID))
		goto out;

	seafile.remote_uid = stbuf.st_uid;
	seafile.local_uid = getuid();
	seafile.remote_gid = stbuf.st_gid;
	seafile.local_gid = getgid();
	seafile.remote_uid_detected = 1;
	DEBUG("remote_uid = %i\n", seafile.remote_uid);

out:
	if (!seafile.remote_uid_detected)
		fprintf(stderr, "failed to detect remote user ID\n");

	buf_free(&buf);
}

static int sftp_check_root(const char *base_path)
{
	int flags;
	uint32_t id = sftp_get_id();
	uint32_t replid;
	uint8_t type;
	struct buffer buf;
	struct stat stbuf;
	struct iovec iov[1];
	int err = -1;
	const char *remote_dir = base_path[0] ? base_path : ".";

	buf_init(&buf, 0);
	buf_add_string(&buf, remote_dir);
	buf_to_iov(&buf, &iov[0]);
	if (sftp_send_iov(SSH_FXP_LSTAT, id, iov, 1) == -1)
		goto out;
	buf_clear(&buf);
	if (sftp_read(&type, &buf) == -1)
		goto out;
	if (type != SSH_FXP_ATTRS && type != SSH_FXP_STATUS) {
		fprintf(stderr, "protocol error\n");
		goto out;
	}
	if (buf_get_uint32(&buf, &replid) == -1)
		goto out;
	if (replid != id) {
		fprintf(stderr, "bad reply ID\n");
		goto out;
	}
	if (type == SSH_FXP_STATUS) {
		uint32_t serr;
		if (buf_get_uint32(&buf, &serr) == -1)
			goto out;

		fprintf(stderr, "%s:%s: %s\n", seafile.host, remote_dir,
			strerror(sftp_error_to_errno(serr)));

		goto out;
	}

	int err2 = buf_get_attrs(&buf, &stbuf, &flags);
	if (err2) {
		err = err2;
		goto out;
	}

	if (!(flags & SSH_FILEXFER_ATTR_PERMISSIONS))
		goto out;

	if (!S_ISDIR(stbuf.st_mode)) {
		fprintf(stderr, "%s:%s: Not a directory\n", seafile.host,
			remote_dir);
		goto out;
	}

	err = 0;

out:
	buf_free(&buf);
	return err;
}

static int connect_remote(void)
{
	int err;

	if (seafile.slave)
		err = connect_slave();
	else if (seafile.directport)
		err = connect_to(seafile.host, seafile.directport);
	else
		err = start_ssh();
	if (!err)
		err = sftp_init();

	if (err)
		close_conn();
	else
		seafile.num_connect++;

	return err;
}

static int start_processing_thread(void)
{
	int err;
	pthread_t thread_id;
	sigset_t oldset;
	sigset_t newset;

	if (seafile.processing_thread_started)
		return 0;

	if (seafile.rfd == -1) {
		err = connect_remote();
		if (err)
			return -EIO;
	}

	if (seafile.detect_uid) {
		sftp_detect_uid();
		seafile.detect_uid = 0;
	}

	sigemptyset(&newset);
	sigaddset(&newset, SIGTERM);
	sigaddset(&newset, SIGINT);
	sigaddset(&newset, SIGHUP);
	sigaddset(&newset, SIGQUIT);
	pthread_sigmask(SIG_BLOCK, &newset, &oldset);
	err = pthread_create(&thread_id, NULL, process_requests, NULL);
	if (err) {
		fprintf(stderr, "failed to create thread: %s\n", strerror(err));
		return -EIO;
	}
	pthread_detach(thread_id);
	pthread_sigmask(SIG_SETMASK, &oldset, NULL);
	seafile.processing_thread_started = 1;
	return 0;
}

static void *seafile_init(struct fuse_conn_info *conn,
                        struct fuse_config *cfg)
{
	/* Readahead should be done by kernel or seafile but not both */
	if (conn->capable & FUSE_CAP_ASYNC_READ)
		seafile.sync_read = 1;

	// These workarounds require the "path" argument.
        cfg->nullpath_ok = ~(seafile.truncate_workaround || seafile.fstat_workaround);

        // Lookup of . and .. is supported
        conn->capable |= FUSE_CAP_EXPORT_SUPPORT;

	if (!seafile.delay_connect)
		start_processing_thread();

	// SFTP only supports 1-second time resolution
	conn->time_gran = 1000000000;
	
	return NULL;
}

static int sftp_request_wait(struct request *req, uint8_t type,
                             uint8_t expect_type, struct buffer *outbuf)
{
	int err;

	if (req->error) {
		err = req->error;
		goto out;
	}
	while (sem_wait(&req->ready));
	if (req->error) {
		err = req->error;
		goto out;
	}
	err = -EIO;
	if (req->reply_type != expect_type &&
	    req->reply_type != SSH_FXP_STATUS) {
		fprintf(stderr, "protocol error\n");
		goto out;
	}
	if (req->reply_type == SSH_FXP_STATUS) {
		uint32_t serr;
		if (buf_get_uint32(&req->reply, &serr) == -1)
			goto out;

		switch (serr) {
		case SSH_FX_OK:
			if (expect_type == SSH_FXP_STATUS)
				err = 0;
			else
				err = -EIO;
			break;

		case SSH_FX_EOF:
			if (type == SSH_FXP_READ || type == SSH_FXP_READDIR)
				err = MY_EOF;
			else
				err = -EIO;
			break;

		case SSH_FX_FAILURE:
			if (type == SSH_FXP_RMDIR)
				err = -ENOTEMPTY;
			else
				err = -EPERM;
			break;

		default:
			err = -sftp_error_to_errno(serr);
		}
	} else {
		buf_init(outbuf, req->reply.size - req->reply.len);
		buf_get_mem(&req->reply, outbuf->p, outbuf->size);
		err = 0;
	}

out:
	if (req->end_func) {
		pthread_mutex_lock(&seafile.lock);
		req->end_func(req);
		pthread_mutex_unlock(&seafile.lock);
	}
	request_free(req);
	return err;
}

static int sftp_request_send(uint8_t type, struct iovec *iov, size_t count,
                             request_func begin_func, request_func end_func,
                             int want_reply, void *data,
                             struct request **reqp)
{
	int err;
	uint32_t id;
	struct request *req = g_new0(struct request, 1);

	req->want_reply = want_reply;
	req->end_func = end_func;
	req->data = data;
	sem_init(&req->ready, 0, 0);
	buf_init(&req->reply, 0);
	pthread_mutex_lock(&seafile.lock);
	if (begin_func)
		begin_func(req);
	id = sftp_get_id();
	req->id = id;
	err = start_processing_thread();
	if (err) {
		pthread_mutex_unlock(&seafile.lock);
		goto out;
	}
	req->len = iov_length(iov, count) + 9;
	seafile.outstanding_len += req->len;
	while (seafile.outstanding_len > seafile.max_outstanding_len)
		pthread_cond_wait(&seafile.outstanding_cond, &seafile.lock);

	g_hash_table_insert(seafile.reqtab, GUINT_TO_POINTER(id), req);
	if (seafile.debug) {
		gettimeofday(&req->start, NULL);
		seafile.num_sent++;
		seafile.bytes_sent += req->len;
	}
	DEBUG("[%05i] %s\n", id, type_name(type));
	pthread_mutex_unlock(&seafile.lock);

	err = -EIO;
	if (sftp_send_iov(type, id, iov, count) == -1) {
		gboolean rmed;

		pthread_mutex_lock(&seafile.lock);
		rmed = g_hash_table_remove(seafile.reqtab, GUINT_TO_POINTER(id));
		pthread_mutex_unlock(&seafile.lock);

		if (!rmed && !want_reply) {
			/* request already freed */
			return err;
		}
		goto out;
	}
	if (want_reply)
		*reqp = req;
	return 0;

out:
	req->error = err;
	if (!want_reply)
		sftp_request_wait(req, type, 0, NULL);
	else
		*reqp = req;

	return err;
}


static int sftp_request_iov(uint8_t type, struct iovec *iov, size_t count,
                            uint8_t expect_type, struct buffer *outbuf)
{
	int err;
	struct request *req;

	err = sftp_request_send(type, iov, count, NULL, NULL, expect_type, NULL,
				&req);
	if (expect_type == 0)
		return err;

	return sftp_request_wait(req, type, expect_type, outbuf);
}

static int sftp_request(uint8_t type, const struct buffer *buf,
                        uint8_t expect_type, struct buffer *outbuf)
{
	struct iovec iov;

	buf_to_iov(buf, &iov); //把path 复制到iov里面
	return sftp_request_iov(type, &iov, 1, expect_type, outbuf);
}

static int seafile_access(const char *path, int mask)
{
	struct stat stbuf;
	int err = 0;

	if (mask ) {
		err = seafile.op->getattr(path, &stbuf, NULL);
		if (!err) {
			if (S_ISREG(stbuf.st_mode) &&
			    !(stbuf.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)))
				err = -EACCES;
		}
	}
	return err;
}

static int count_components(const char *p)
{
	int ctr;

	for (; *p == '/'; p++);
	for (ctr = 0; *p; ctr++) {
		for (; *p && *p != '/'; p++);
		for (; *p == '/'; p++);
	}
	return ctr;
}

static void strip_common(const char **sp, const char **tp)
{
	const char *s = *sp;
	const char *t = *tp;
	do {
		for (; *s == '/'; s++);
		for (; *t == '/'; t++);
		*tp = t;
		*sp = s;
		for (; *s == *t && *s && *s != '/'; s++, t++);
	} while ((*s == *t && *s) || (!*s && *t == '/') || (*s == '/' && !*t));
}

static void transform_symlink(const char *path, char **linkp)
{
	const char *l = *linkp;
	const char *b = seafile.base_path;
	char *newlink;
	char *s;
	int dotdots;
	int i;

	if (l[0] != '/' || b[0] != '/')
		return;

	strip_common(&l, &b);
	if (*b)
		return;

	strip_common(&l, &path);
	dotdots = count_components(path);
	if (!dotdots)
		return;
	dotdots--;

	newlink = malloc(dotdots * 3 + strlen(l) + 2);
	if (!newlink) {
		fprintf(stderr, "seafile: memory allocation failed\n");
		abort();
	}
	for (s = newlink, i = 0; i < dotdots; i++, s += 3)
		strcpy(s, "../");

	if (l[0])
		strcpy(s, l);
	else if (!dotdots)
		strcpy(s, ".");
	else
		s[0] = '\0';

	free(*linkp);
	*linkp = newlink;
}

static int seafile_readlink(const char *path, char *linkbuf, size_t size)
{
	int err;
	struct buffer buf;
	struct buffer name;

	assert(size > 0);

	if (seafile.server_version < 3)
		return -EPERM;

	buf_init(&buf, 0);
	buf_add_path(&buf, path);
	err = sftp_request(SSH_FXP_READLINK, &buf, SSH_FXP_NAME, &name);
	if (!err) {
		uint32_t count;
		char *link;
		err = -EIO;
		if(buf_get_uint32(&name, &count) != -1 && count == 1 &&
		   buf_get_string(&name, &link) != -1) {
			if (seafile.transform_symlinks)
				transform_symlink(path, &link);
			strncpy(linkbuf, link, size - 1);
			linkbuf[size - 1] = '\0';
			free(link);
			err = 0;
		}
		buf_free(&name);
	}
	buf_free(&buf);
	return err;
}

static int sftp_readdir_send(struct request **req, struct buffer *handle)
{
	struct iovec iov;

	buf_to_iov(handle, &iov);
	return sftp_request_send(SSH_FXP_READDIR, &iov, 1, NULL, NULL,
				 SSH_FXP_NAME, NULL, req);
}

static int seafile_req_pending(struct request *req)
{
	if (g_hash_table_lookup(seafile.reqtab, GUINT_TO_POINTER(req->id)))
		return 1;
	else
		return 0;
}

static int sftp_readdir_async(struct buffer *handle, void *buf, off_t offset,
			     fuse_fill_dir_t filler)
{
	int err = 0;
	int outstanding = 0;
	int max = READDIR_START;
	GList *list = NULL;

	int done = 0;

	assert(offset == 0);
	while (!done || outstanding) {
		struct request *req;
		struct buffer name;
		int tmperr;

		while (!done && outstanding < max) {
			tmperr = sftp_readdir_send(&req, handle);

			if (tmperr && !done) {
				err = tmperr;
				done = 1;
				break;
			}

			list = g_list_append(list, req);
			outstanding++;
		}

		if (outstanding) {
			GList *first;
			/* wait for response to next request */
			first = g_list_first(list);
			req = first->data;
			list = g_list_delete_link(list, first);
			outstanding--;

			if (done) {
				/* We need to cache want_reply, since processing
				   thread may free req right after unlock() if
				   want_reply == 0 */
				int want_reply;
				pthread_mutex_lock(&seafile.lock);
				if (seafile_req_pending(req))
					req->want_reply = 0;
				want_reply = req->want_reply;
				pthread_mutex_unlock(&seafile.lock);
				if (!want_reply)
					continue;
			}

			tmperr = sftp_request_wait(req, SSH_FXP_READDIR,
						    SSH_FXP_NAME, &name);

			if (tmperr && !done) {
				err = tmperr;
				if (err == MY_EOF)
					err = 0;
				done = 1;
			}
			if (!done) {
				err = buf_get_entries(&name, buf, filler);
				buf_free(&name);

				/* increase number of outstanding requests */
				if (max < READDIR_MAX)
					max++;

				if (err)
					done = 1;
			}
		}
	}
	assert(list == NULL);

	return err;
}

static int sftp_readdir_sync(struct buffer *handle, void *buf, off_t offset,
			     fuse_fill_dir_t filler)
{
	int err;
	assert(offset == 0);
	do {
		struct buffer name;
		err = sftp_request(SSH_FXP_READDIR, handle, SSH_FXP_NAME, &name);
		if (!err) {
			err = buf_get_entries(&name, buf, filler);
			buf_free(&name);
		}
	} while (!err);
	if (err == MY_EOF)
		err = 0;

	return err;
}
struct manifest_content{
	char* name = new char[NAME_MAX];
	char* eid = new char[20];
};
struct seafile_dirp {
	struct manifest_content* mc;
	struct seafile_dirent *entry;
	off_t offset;
	size_t eid_nums;

};
struct seafile_dirent
{
	char* inode_id; /* inode number 索引节点号 */
	off_t d_off; /* offset to this dirent 在目录文件中的偏移 */
	unsigned short d_reclen; /* length of this d_name 文件名长 */
	unsigned char d_type; /* the type of d_name 文件类型 */
	char d_name [NAME_MAX+1]; /* file name (null-terminated) 文件名，最长255字符 */
}
static int change_path_to_eid(const char *path,char eid[20]){

	strcpy(eid,"10011");//随便计算一个eid
	return 1;

}
static int seafile_opendir(const char *path, struct fuse_file_info *fi)
{
	int err;
	struct buffer buf;
	struct buffer *handle;
	handle = (struct buffer *)malloc(sizeof(struct buffer));
	if(handle == NULL)
		return -ENOMEM;

	buf_init(&buf, 0);
	buf_add_path(&buf, path);
	char* real_path;
	buf_get_string(&buf,&real_path);
	char* inode_eid = new char[20];
	change_path_to_eid(&buf,inode_eid);

	struct seafile_inode * inode_content =(struct seafile_inode *) 
	malloc(sizeof(struct seafile_inode));
	Seanetfs_getfile(inode_eid,(char*)inode_content);

	struct seafile_dirp *sd = (struct seafile_dirp *)malloc(sizeof(struct seafile_dirp));
	sd->mc = (struct manifest_content*)malloc(sizeof(manifest_content) *inode_content->size );
	Seanetfs_getfile(inode_content->root_manifest_eid,(char*)sd->mc)
	sd->eid_nums = inode_content->size;
	sd->offset = 0;
	sd->dir_inode = inode_content;
	fi->fh = (unsigned long) sd;

	buf_free(&buf);
	free(real_path)
	return 0;
}

static int seafile_readdir(const char *path, void *dbuf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi,
			 enum fuse_readdir_flags flags)
{
	(void) path; (void) flags;
	int err;
	struct seafile_dirp *sd;
	sd = (struct seafile_dirp *)(uintptr_t)fi->fh;

	sd->entry = NULL;
	sd->offset = offset;
	
	if (offset > sd->eid_nums){
		return 0;//需要弄明白如何处理这种错误
	}
	while (1) {
		struct stat st;
		off_t nextoff;
		enum fuse_fill_dir_flags fill_flags = 0;
		char* current_dir_eid = (char* current_dir_eid)malloc(sizeof(char[20]));
		strncpy(current_dir_eid,sd->eids+sd->offset*20,20);

		if (!sd->entry) {
			// struct seafile_inode* sf = new struct seafile_inode;	
			// Seanetfs_getfile(current_dir_eid,(char*)sf);
			sd->entry = (struct seafile_dirent *)malloc(sizeof(struct seafile_dirent));
			sd->entry->inode_id = current_dir_eid;
			sd->entry->d_name = sd->mc[offset]->name; 
		}
		
		struct seafile_inode * temp_inode = (struct seafile_inode *)
													malloc(sizeof(struct seafile_inode));

		
	
		Seanetfs_getfile(sd->entry->inode_id,(char*)temp_inode);
		
		st->st_atime = temp_inode->atime;
		st->st_ctime = temp_inode->ctime;
		st->st_dev = temp_inode->dev;
		st->st_gid = temp_inode->gid;
		st->st_ino = sd->entry->inode_id;
		st->st_mode = temp_inode->mode;//不确定这个是否成立。 
		st->st_mtime = temp_inode->mtime;
		st->st_nlink = temp_inode->n_link;
		st->st_size = temp_inode->bit_size;
		st->st_uid = temp_inode->uid;
		
		nextoff = offset + 1;
		if (filler(buf, sd->entry->d_name, &st, nextoff, fill_flags))
			break;
		free(sd->entry);
		
		sd->offset = nextoff;
		}
	return 0;
}
static int seafile_releasedir(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	int err;
	struct seafile_dirp * sd;
	sd = (struct seafile_dirp *)(uintptr_t)fi->fh;
	free(sd);
	return err;
}


static int seafile_mkdir(const char *path, mode_t mode)
{
	int err;
	struct buffer buf;
	buf_init(&buf, 0);
	buf_add_path(&buf, path);
	buf_add_uint32(&buf, SSH_FILEXFER_ATTR_PERMISSIONS);
	buf_add_uint32(&buf, mode);
	err = sftp_request(SSH_FXP_MKDIR, &buf, SSH_FXP_STATUS, NULL);
	buf_free(&buf);
	return err;
}

static int seafile_rmdir(const char *path)
{
	int err;
	struct buffer buf;
	buf_init(&buf, 0);
	buf_add_path(&buf, path);
	err = sftp_request(SSH_FXP_RMDIR, &buf, SSH_FXP_STATUS, NULL);
	buf_free(&buf);
	return err;
}

static int seafile_rename(const char *from, const char *to, unsigned int flags)
{
	int err;

	if(flags != 0)
		return -EINVAL;
	
	if (seafile.ext_posix_rename)
		err = seafile_ext_posix_rename(from, to);
	else
		err = seafile_do_rename(from, to);
	if (err == -EPERM && seafile.rename_workaround) {
		size_t tolen = strlen(to);
		if (tolen + RENAME_TEMP_CHARS < PATH_MAX) {
			int tmperr;
			char totmp[PATH_MAX];
			strcpy(totmp, to);
			random_string(totmp + tolen, RENAME_TEMP_CHARS);
			tmperr = seafile_do_rename(to, totmp);
			if (!tmperr) {
				err = seafile_do_rename(from, to);
				if (!err)
					err = seafile_unlink(totmp);
				else
					seafile_do_rename(totmp, to);
			}
		}
	}
	if (err == -EPERM && seafile.renamexdev_workaround)
		err = -EXDEV;
	return err;
}

static inline struct seafile_inode *get_seafile_inode(struct fuse_file_info *fi)
{
	return (struct seafile_inode *) (uintptr_t) fi->fh;
}

struct context{
	char ** eid_list;
	int chunk_offset;//chunk 起始点 
	int chunk_nums;//本次操作的chunk数量 
	
};
//open 根据path 获取到file的inode 
static int seafile_open(const char *path, struct fuse_file_info *fi)
{
	char* inode_eid = new char[20];
	change_path_to_eid(path,inode_eid);
	int res;
	struct seafile_inode * si = (struct seafile_inode *)malloc(sizeof(struct seafile_inode));
	res = Seanetfs_getfile(inode_eid,(char*)si);

	if(res == 0) return 0;
	struct context * ctx = (struct context*)malloc(sizeof(struct context ctx));
	Seanetfs_init_context(si->root_manifest_eid,ctx);
	si->ctx = ctx;
	fi->fh = (unsigned long) si;
	return 1;
	 
}

static int seafile_flush(const char *path, struct fuse_file_info *fi)
{
	int err;
	struct seafile_inode *sf = get_seafile_inode(fi);
	struct list_head write_reqs;
	struct list_head *curr_list;

	if (!seafile_file_is_conn(sf))
		return -EIO;

	if (seafile.sync_write)
		return 0;

	(void) path;
	pthread_mutex_lock(&seafile.lock);
	if (!list_empty(&sf->write_reqs)) {
		curr_list = sf->write_reqs.prev;
		list_del(&sf->write_reqs);
		list_init(&sf->write_reqs);
		list_add(&write_reqs, curr_list);
		while (!list_empty(&write_reqs))
			pthread_cond_wait(&sf->write_finished, &seafile.lock);
	}
	err = sf->write_error;
	sf->write_error = 0;
	pthread_mutex_unlock(&seafile.lock);
	return err;
}


static void seafile_file_put(struct seafile_inode *sf)
{
	sf->refs--;
	if (!sf->refs)
		g_free(sf);
}

static void seafile_file_get(struct seafile_inode *sf)
{
	sf->refs++;
}

static int seafile_release(const char *path, struct fuse_file_info *fi)
{
	struct seafile_inode *sf = get_seafile_inode(fi);
	struct buffer *handle = &sf->handle;
	if (seafile_file_is_conn(sf)) {
		seafile_flush(path, fi);
		sftp_request(SSH_FXP_CLOSE, handle, 0, NULL);
	}
	buf_free(handle);
	chunk_put_locked(sf->readahead);
	seafile_file_put(sf);
	return 0;
}



static int seafile_read(const char *path, char *rbuf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
	struct seafile_inode *sf = get_seafile_inode(fi);
	int chunk_offset = int(offset/CHUNK_SIZE);
	int chunk_num = int((offset + size)/CHUNK_SIZE) + 1 - chunk_offset;
	
	char * temp_content = (char *)malloc(sizeof(char)*chunk_num * CHUNK_SIZE)
	sf->ctx->chunk_offset = chunk_offset;
	
	st->ctx->chunk_nums = chunk_num;
	Seanetfs_getfile(NULL,);
	
}


static int seafile_write(const char *path, const char *wbuf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
	int err;
	struct seafile_inode *sf = get_seafile_inode(fi);

	(void) path;

	if (!seafile_file_is_conn(sf))
		return -EIO;

	seafile_inc_modifver();

	// if (!seafile.sync_write && !sf->write_error)
	// 	err = seafile_async_write(sf, wbuf, size, offset);
	// else
	// 	err = seafile_sync_write(sf, wbuf, size, offset);

	return err ? err : (int) size;
}

static int seafile_ext_statvfs(const char *path, struct statvfs *stbuf)
{
	int err;
	struct buffer buf;
	struct buffer outbuf;
	buf_init(&buf, 0);
	buf_add_string(&buf, SFTP_EXT_STATVFS);
	buf_add_path(&buf, path);
	err = sftp_request(SSH_FXP_EXTENDED, &buf, SSH_FXP_EXTENDED_REPLY,
			   &outbuf);
	if (!err) {
		if (buf_get_statvfs(&outbuf, stbuf) == -1)
			err = -EIO;
		buf_free(&outbuf);
	}
	buf_free(&buf);
	return err;
}


static int seafile_statfs(const char *path, struct statvfs *buf)
{
	if (seafile.ext_statvfs)
		return seafile_ext_statvfs(path, buf);

	buf->f_namemax = 255;
	buf->f_bsize = seafile.blksize;
	/*
	 * df seems to use f_bsize instead of f_frsize, so make them
	 * the same
	 */
	buf->f_frsize = buf->f_bsize;
	buf->f_blocks = buf->f_bfree =  buf->f_bavail =
		1000ULL * 1024 * 1024 * 1024 / buf->f_frsize;
	buf->f_files = buf->f_ffree = 1000000000;
	return 0;
}

static int seafile_create(const char *path, mode_t mode,
                        struct fuse_file_info *fi)
{
	if (seafile.createmode_workaround)
		mode = 0;

	return seafile_open_common(path, mode, fi);
}


static int seafile_getattr(const char *path, struct stat *stbuf,
			 struct fuse_file_info *fi)
{
	int err;

	struct seafile_inode *sf = NULL;

	if (fi != NULL && !seafile.fstat_workaround) {
		sf = get_seafile_inode(fi);
	}
	
	char* inode_eid = new char[20];

	if(sf == NULL) {

		if(change_path_to_eid(buf,&inode_eid)){
			if(Seanetfs_getfile(inode_eid,(char *)sf) == 0){
				return -EIO
			}
		}
	}
	if (!err) {
		err = buf_get_attrs(&sf, stbuf, NULL);
#ifdef __APPLE__
		stbuf->st_blksize = 0;
#endif
	}
	buf_free(&buf);
	return err;
}

static int seafile_truncate_zero(const char *path)
{
	int err;
	struct fuse_file_info fi;

	fi.flags = O_WRONLY | O_TRUNC;
	err = seafile_open(path, &fi);
	if (!err)
		seafile_release(path, &fi);

	return err;
}

static size_t calc_buf_size(off_t size, off_t offset)
{
	return offset + seafile.max_read < size ? seafile.max_read : size - offset;
}

static int seafile_truncate_shrink(const char *path, off_t size)
{
	int res;
	char *data;
	off_t offset;
	struct fuse_file_info fi;

	data = calloc(size, 1);
	if (!data)
		return -ENOMEM;

	fi.flags = O_RDONLY;
	res = seafile_open(path, &fi);
	if (res)
		goto out;

	for (offset = 0; offset < size; offset += res) {
		size_t bufsize = calc_buf_size(size, offset);
		res = seafile_read(path, data + offset, bufsize, offset, &fi);
		if (res <= 0)
			break;
	}
	seafile_release(path, &fi);
	if (res < 0)
		goto out;

	fi.flags = O_WRONLY | O_TRUNC;
	res = seafile_open(path, &fi);
	if (res)
		goto out;

	for (offset = 0; offset < size; offset += res) {
		size_t bufsize = calc_buf_size(size, offset);
		res = seafile_write(path, data + offset, bufsize, offset, &fi);
		if (res < 0)
			break;
	}
	if (res >= 0)
		res = seafile_flush(path, &fi);
	seafile_release(path, &fi);

out:
	free(data);
	return res;
}

static int seafile_truncate_extend(const char *path, off_t size,
                                 struct fuse_file_info *fi)
{
	int res;
	char c = 0;
	struct fuse_file_info tmpfi;
	struct fuse_file_info *openfi = fi;
	if (!fi) {
		openfi = &tmpfi;
		openfi->flags = O_WRONLY;
		res = seafile_open(path, openfi);
		if (res)
			return res;
	}
	res = seafile_write(path, &c, 1, size - 1, openfi);
	if (res == 1)
		res = seafile_flush(path, openfi);
	if (!fi)
		seafile_release(path, openfi);

	return res;
}

/*
 * Work around broken sftp servers which don't handle
 * SSH_FILEXFER_ATTR_SIZE in SETSTAT request.
 *
 * If new size is zero, just open the file with O_TRUNC.
 *
 * If new size is smaller than current size, then copy file locally,
 * then open/trunc and send it back.
 *
 * If new size is greater than current size, then write a zero byte to
 * the new end of the file.
 */
static int seafile_truncate_workaround(const char *path, off_t size,
                                     struct fuse_file_info *fi)
{
	if (size == 0)
		return seafile_truncate_zero(path);
	else {
		struct stat stbuf;
		int err;
		err = seafile_getattr(path, &stbuf, fi);
		if (err)
			return err;
		if (stbuf.st_size == size)
			return 0;
		else if (stbuf.st_size > size)
			return seafile_truncate_shrink(path, size);
		else
			return seafile_truncate_extend(path, size, fi);
	}
}

static int processing_init(void)
{
	signal(SIGPIPE, SIG_IGN);

	pthread_mutex_init(&seafile.lock, NULL);
	pthread_mutex_init(&seafile.lock_write, NULL);
	pthread_cond_init(&seafile.outstanding_cond, NULL);
	seafile.reqtab = g_hash_table_new(NULL, NULL);
	if (!seafile.reqtab) {
		fprintf(stderr, "failed to create hash table\n");
		return -1;
	}
	return 0;
}

static struct fuse_operations seafile_oper = {
		.init       = seafile_init,
		.getattr    = seafile_getattr,
		.access     = seafile_access,
		.opendir    = seafile_opendir,
		.readdir    = seafile_readdir,
		.releasedir = seafile_releasedir,
		.readlink   = seafile_readlink,
		.mknod      = seafile_mknod,
		.mkdir      = seafile_mkdir,
		.symlink    = seafile_symlink,
		.unlink     = seafile_unlink,
		.rmdir      = seafile_rmdir,
		.rename     = seafile_rename,
		.link       = seafile_link,
		.chmod      = seafile_chmod,
		.chown      = seafile_chown,
		.truncate   = seafile_truncate,
		.utimens    = seafile_utimens,
		.open       = seafile_open,
		.flush      = seafile_flush,
		.fsync      = seafile_fsync,
		.release    = seafile_release,
		.read       = seafile_read,
		.write      = seafile_write,
		.statfs     = seafile_statfs,
		.create     = seafile_create,
};

static void usage(const char *progname)
{
	printf(
"usage: %s [user@]host:[dir] mountpoint [options]\n"
"\n"
"    -h   --help            print help\n"
"    -V   --version         print version\n"
"    -f                     foreground operation\n"
"    -s                     disable multi-threaded operation\n"
"    -p PORT                equivalent to '-o port=PORT'\n"
"    -C                     equivalent to '-o compression=yes'\n"
"    -F ssh_configfile      specifies alternative ssh configuration file\n"
"    -1                     equivalent to '-o ssh_protocol=1'\n"
"    -o opt,[opt...]        mount options\n"
"    -o reconnect           reconnect to server\n"
"    -o delay_connect       delay connection to server\n"
"    -o seafile_sync          synchronous writes\n"
"    -o no_readahead        synchronous reads (no speculative readahead)\n"
"    -o sync_readdir        synchronous readdir\n"
"    -d, --debug            print some debugging information (implies -f)\n"
"    -v, --verbose          print ssh replies and messages\n"
"    -o dir_cache=BOOL      enable caching of directory contents (names,\n"
"                           attributes, symlink targets) {yes,no} (default: yes)\n"
"    -o dcache_max_size=N   sets the maximum size of the directory cache (default: 10000)\n"
"    -o dcache_timeout=N    sets timeout for directory cache in seconds (default: 20)\n"
"    -o dcache_{stat,link,dir}_timeout=N\n"
"                           sets separate timeout for {attributes, symlinks, names}\n"
"    -o dcache_clean_interval=N\n"
"                           sets the interval for automatic cleaning of the\n"
"                           cache (default: 60)\n"
"    -o dcache_min_clean_interval=N\n"
"                           sets the interval for forced cleaning of the\n"
"                           cache if full (default: 5)\n"
"    -o direct_io           enable direct i/o\n"
"    -o workaround=LIST     colon separated list of workarounds\n"
"             none             no workarounds enabled\n"
"             [no]rename       fix renaming to existing file (default: off)\n"
"             [no]renamexdev   fix moving across filesystems (default: off)\n"
"             [no]truncate     fix truncate for old servers (default: off)\n"
"             [no]buflimit     fix buffer fillup bug in server (default: on)\n"
"             [no]fstat        always use stat() instead of fstat() (default: off)\n"
"             [no]createmode   always pass mode 0 to create (default: off)\n"
"    -o idmap=TYPE          user/group ID mapping (default: " IDMAP_DEFAULT ")\n"
"             none             no translation of the ID space\n"
"             user             only translate UID/GID of connecting user\n"
"             file             translate UIDs/GIDs contained in uidfile/gidfile\n"
"    -o uidfile=FILE        file containing username:remote_uid mappings\n"
"    -o gidfile=FILE        file containing groupname:remote_gid mappings\n"
"    -o nomap=TYPE          with idmap=file, how to handle missing mappings\n"
"             ignore           don't do any re-mapping\n"
"             error            return an error (default)\n"
"    -o ssh_command=CMD     execute CMD instead of 'ssh'\n"
"    -o ssh_protocol=N      ssh protocol to use (default: 2)\n"
"    -o sftp_server=SERV    path to sftp server or subsystem (default: sftp)\n"
"    -o directport=PORT     directly connect to PORT bypassing ssh\n"
"    -o slave               communicate over stdin and stdout bypassing network\n"
"    -o disable_hardlink    link(2) will return with errno set to ENOSYS\n"
"    -o transform_symlinks  transform absolute symlinks to relative\n"
"    -o follow_symlinks     follow symlinks on the server\n"
"    -o no_check_root       don't check for existence of 'dir' on server\n"
"    -o password_stdin      read password from stdin (only for pam_mount!)\n"
"    -o SSHOPT=VAL          ssh options (see man ssh_config)\n"
"\n"
"FUSE Options:\n",
progname);
}

static int is_ssh_opt(const char *arg)
{
	if (arg[0] != '-') {
		unsigned arglen = strlen(arg);
		const char **o;
		for (o = ssh_opts; *o; o++) {
			unsigned olen = strlen(*o);
			if (arglen > olen && arg[olen] == '=' &&
			    strncasecmp(arg, *o, olen) == 0)
				return 1;
		}
	}
	return 0;
}

static int seafile_opt_proc(void *data, const char *arg, int key,
                          struct fuse_args *outargs)
{
	(void) outargs; (void) data;
	char *tmp;

	switch (key) {
	case FUSE_OPT_KEY_OPT:
		if (is_ssh_opt(arg)) {
			tmp = g_strdup_printf("-o%s", arg);
			ssh_add_arg(tmp);
			g_free(tmp);
			return 0;
		}
		/* Pass through */
		return 1;

	case FUSE_OPT_KEY_NONOPT:
		if (!seafile.host && strchr(arg, ':')) {
			seafile.host = strdup(arg);
			return 0;
		}
		else if (!seafile.mountpoint) {
#if defined(__CYGWIN__)
			/*
			 * On FUSE for Cygwin the mountpoint may be a drive or directory.
			 * Furthermore the mountpoint must NOT exist prior to mounting.
			 * So we cannot use realpath(3).
			 */
			if ((('A' <= arg[0] && arg[0] <= 'Z') || ('a' <= arg[0] && arg[0] <= 'z'))
				&& ':' == arg[1] && '\0' == arg[2]) {
				/* drive: make a copy */
				seafile.mountpoint = strdup(arg);
			} else {
				/* path: split into dirname, basename and check dirname */
				char *dir;
				const char *base;
				const char *slash = strrchr(arg, '/');
				if (slash) {
					char *tmp = strndup(arg, slash == arg ? 1 : slash - arg);
					dir = tmp ? realpath(tmp, NULL) : 0;
					base = slash + 1;
					free(tmp);
				} else {
					dir = realpath(".", NULL);
					base = arg;
				}
				if (dir) {
					slash = '/' == dir[0] && '\0' == dir[1] ? "" : "/";
					asprintf(&seafile.mountpoint, "%s%s%s", dir, slash, base);
					free(dir);
				}
			}
#else
                        int fd, len;
                        if (sscanf(arg, "/dev/fd/%u%n", &fd, &len) == 1 &&
                            len == strlen(arg)) {
                                /*
                                 * Allow /dev/fd/N unchanged; it can be
                                 * use for pre-mounting a generic fuse
                                 * mountpoint to later be completely
                                 * unprivileged with libfuse >= 3.3.0.
                                 */
                                seafile.mountpoint = arg;
                        } else {
                                seafile.mountpoint = realpath(arg, NULL);
                        }
#endif
			if (!seafile.mountpoint) {
				fprintf(stderr, "seafile: bad mount point `%s': %s\n",
					arg, strerror(errno));
				return -1;
			}
			return 0;
		}
		fprintf(stderr, "seafile: invalid argument `%s'\n", arg);
		return -1;


	case KEY_PORT:
		tmp = g_strdup_printf("-oPort=%s", arg + 2);
		ssh_add_arg(tmp);
		g_free(tmp);
		return 0;

	case KEY_COMPRESS:
		ssh_add_arg("-oCompression=yes");
		return 0;

	case KEY_CONFIGFILE:
		tmp = g_strdup_printf("-F%s", arg + 2);
		ssh_add_arg(tmp);
		g_free(tmp);
		return 0;

	default:
		fprintf(stderr, "internal error\n");
		abort();
	}
}

static int workaround_opt_proc(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	(void) data; (void) key; (void) outargs;
	fprintf(stderr, "unknown workaround: '%s'\n", arg);
	return -1;
}

static int parse_workarounds(void)
{
	int res;
        /* Need separate variables because literals are const
           char */
        char argv0[] = "";
        char argv1[] = "-o";
	char *argv[] = { argv0, argv1, seafile.workarounds, NULL };
	struct fuse_args args = FUSE_ARGS_INIT(3, argv);
	char *s = seafile.workarounds;
	if (!s)
		return 0;

	while ((s = strchr(s, ':')))
		*s = ',';

	res = fuse_opt_parse(&args, &seafile, workaround_opts,
			     workaround_opt_proc);
	fuse_opt_free_args(&args);

	return res;
}

static int read_password(void)
{
	int size = getpagesize();
	int max_password = MIN(MAX_PASSWORD, size - 1);
	int n;

	seafile.password = mmap(NULL, size, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED,
			      -1, 0);
	if (seafile.password == MAP_FAILED) {
		perror("Failed to allocate locked page for password");
		return -1;
	}
	if (mlock(seafile.password, size) != 0) {
		memset(seafile.password, 0, size);
		munmap(seafile.password, size);
		seafile.password = NULL;
		perror("Failed to allocate locked page for password");
		return -1;
	}

	/* Don't use fgets() because password might stay in memory */
	for (n = 0; n < max_password; n++) {
		int res;

		res = read(0, &seafile.password[n], 1);
		if (res == -1) {
			perror("Reading password");
			return -1;
		}
		if (res == 0) {
			seafile.password[n] = '\n';
			break;
		}
		if (seafile.password[n] == '\n')
			break;
	}
	if (n == max_password) {
		fprintf(stderr, "Password too long\n");
		return -1;
	}
	seafile.password[n+1] = '\0';
	ssh_add_arg("-oNumberOfPasswordPrompts=1");

	return 0;
}

// Behaves similarly to strtok(), but allows for the ' ' delimiter to be escaped
// by '\ '.
static char *tokenize_on_space(char *str)
{
	static char *pos = NULL;
	char *start = NULL;

	if (str)
		pos = str;

	if (!pos)
		return NULL;

	// trim any leading spaces
	while (*pos == ' ')
		pos++;

	start = pos;

	while (pos && *pos != '\0') {
		// break on space, but not on '\ '
		if (*pos == ' ' && *(pos - 1) != '\\') {
			break;
		}
		pos++;
	}

	if (*pos == '\0') {
		pos = NULL;
	}
	else {
		*pos = '\0';
		pos++;
	}

	return start;
}

static void set_ssh_command(void)
{
	char *token = NULL;
	int i = 0;

	token = tokenize_on_space(seafile.ssh_command);
	while (token != NULL) {
		if (i == 0) {
			replace_arg(&seafile.ssh_args.argv[0], token);
		} else {
			if (fuse_opt_insert_arg(&seafile.ssh_args, i, token) == -1)
				_exit(1);
		}
		i++;

		token = tokenize_on_space(NULL);
	}
}

static char *find_base_path(void)
{
	char *s = seafile.host;
	char *d = s;

	for (; *s && *s != ':'; s++) {
		if (*s == '[') {
			/*
			 * Handle IPv6 numerical address enclosed in square
			 * brackets
			 */
			s++;
			for (; *s != ']'; s++) {
				if (!*s) {
					fprintf(stderr,	"missing ']' in hostname\n");
					exit(1);
				}
				*d++ = *s;
			}
		} else {
			*d++ = *s;
		}

	}
	*d++ = '\0';
	s++;

	return s;
}

static char *fsname_escape_commas(char *fsnameold)
{
	char *fsname = g_malloc(strlen(fsnameold) * 2 + 1);
	char *d = fsname;
	char *s;

	for (s = fsnameold; *s; s++) {
		if (*s == '\\' || *s == ',')
			*d++ = '\\';
		*d++ = *s;
	}
	*d = '\0';
	g_free(fsnameold);

	return fsname;
}

static int ssh_connect(void)
{
	int res;

	res = processing_init();
	if (res == -1)
		return -1;

	if (!seafile.delay_connect) {
		if (connect_remote() == -1)
			return -1;

		if (!seafile.no_check_root &&
		    sftp_check_root(seafile.base_path) != 0)
			return -1;

	}
	return 0;
}

/* number of ':' separated fields in a passwd/group file that we care
 * about */
#define IDMAP_FIELDS 3

/* given a line from a uidmap or gidmap, parse out the name and id */
static void parse_idmap_line(char *line, const char* filename,
		const unsigned int lineno, uint32_t *ret_id, char **ret_name,
		const int eof)
{
	/* chomp off the trailing newline */
	char *p = line;
	if ((p = strrchr(line, '\n')))
		*p = '\0';
	else if (!eof) {
		fprintf(stderr, "%s:%u: line too long\n", filename, lineno);
		exit(1);
	}
	char *tokens[IDMAP_FIELDS];
	char *tok;
	int i;
	for (i = 0; (tok = strsep(&line, ":")) && (i < IDMAP_FIELDS) ; i++) {
		tokens[i] = tok;
	}

	char *name_tok, *id_tok;
	if (i == 2) {
		/* assume name:id format */
		name_tok = tokens[0];
		id_tok = tokens[1];
	} else if (i >= IDMAP_FIELDS) {
		/* assume passwd/group file format */
		name_tok = tokens[0];
		id_tok = tokens[2];
	} else {
		fprintf(stderr, "%s:%u: unknown format\n", filename, lineno);
		exit(1);
	}

	errno = 0;
	uint32_t remote_id = strtoul(id_tok, NULL, 10);
	if (errno) {
		fprintf(stderr, "Invalid id number on line %u of '%s': %s\n",
				lineno, filename, strerror(errno));
		exit(1);
	}

	*ret_name = strdup(name_tok);
	*ret_id = remote_id;
}

/* read a uidmap or gidmap */
static void read_id_map(char *file, uint32_t *(*map_fn)(char *),
		const char *name_id, GHashTable **idmap, GHashTable **r_idmap)
{
	*idmap = g_hash_table_new(NULL, NULL);
	*r_idmap = g_hash_table_new(NULL, NULL);
	FILE *fp;
	char line[LINE_MAX];
	unsigned int lineno = 0;
	uid_t local_uid = getuid();

	fp = fopen(file, "r");
	if (fp == NULL) {
		fprintf(stderr, "failed to open '%s': %s\n",
				file, strerror(errno));
		exit(1);
	}
	struct stat st;
	if (fstat(fileno(fp), &st) == -1) {
		fprintf(stderr, "failed to stat '%s': %s\n", file,
				strerror(errno));
		exit(1);
	}
	if (st.st_uid != local_uid) {
		fprintf(stderr, "'%s' is not owned by uid %lu\n", file,
				(unsigned long)local_uid);
		exit(1);
	}
	if (st.st_mode & S_IWGRP || st.st_mode & S_IWOTH) {
		fprintf(stderr, "'%s' is writable by other users\n", file);
		exit(1);
	}

	while (fgets(line, LINE_MAX, fp) != NULL) {
		lineno++;
		uint32_t remote_id;
		char *name;

		/* skip blank lines */
		if (line[0] == '\n' || line[0] == '\0')
			continue;

		parse_idmap_line(line, file, lineno, &remote_id, &name, feof(fp));

		uint32_t *local_id = map_fn(name);
		if (local_id == NULL) {
			/* not found */
			DEBUG("%s(%u): no local %s\n", name, remote_id, name_id);
			free(name);
			continue;
		}

		DEBUG("%s: remote %s %u => local %s %u\n",
				name, name_id, remote_id, name_id, *local_id);
		g_hash_table_insert(*idmap, GUINT_TO_POINTER(remote_id), GUINT_TO_POINTER(*local_id));
		g_hash_table_insert(*r_idmap, GUINT_TO_POINTER(*local_id), GUINT_TO_POINTER(remote_id));
		free(name);
		free(local_id);
	}

	if (fclose(fp) == EOF) {
		fprintf(stderr, "failed to close '%s': %s",
				file, strerror(errno));
		exit(1);
	}
}

/* given a username, return a pointer to its uid, or NULL if it doesn't
 * exist on this system */
static uint32_t *username_to_uid(char *name)
{
	errno = 0;
	struct passwd *pw = getpwnam(name);
	if (pw == NULL) {
		if (errno == 0) {
			/* "does not exist" */
			return NULL;
		}
		fprintf(stderr, "Failed to look up user '%s': %s\n",
				name, strerror(errno));
		exit(1);
	}
	uint32_t *r = malloc(sizeof(uint32_t));
	if (r == NULL) {
		fprintf(stderr, "seafile: memory allocation failed\n");
		abort();
	}
	*r = pw->pw_uid;
	return r;
}

/* given a groupname, return a pointer to its gid, or NULL if it doesn't
 * exist on this system */
static uint32_t *groupname_to_gid(char *name)
{
	errno = 0;
	struct group *gr = getgrnam(name);
	if (gr == NULL) {
		if (errno == 0) {
			/* "does not exist" */
			return NULL;
		}
		fprintf(stderr, "Failed to look up group '%s': %s\n",
				name, strerror(errno));
		exit(1);
	}
	uint32_t *r = malloc(sizeof(uint32_t));
	if (r == NULL) {
		fprintf(stderr, "seafile: memory allocation failed\n");
		abort();
	}
	*r = gr->gr_gid;
	return r;
}

static inline void load_uid_map(void)
{
	read_id_map(seafile.uid_file, &username_to_uid, "uid", &seafile.uid_map, &seafile.r_uid_map);
}

static inline void load_gid_map(void)
{
	read_id_map(seafile.gid_file, &groupname_to_gid, "gid", &seafile.gid_map, &seafile.r_gid_map);
}

#ifdef __APPLE__
int main(int argc, char *argv[], __unused char *envp[], char **exec_path)
#else
int main(int argc, char *argv[])
#endif
{
	int res;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char *tmp;
	char *fsname;
	const char *sftp_server;
	struct fuse *fuse;
	struct fuse_session *se;

#ifdef __APPLE__
	if (!realpath(*exec_path, seafile_program_path)) {
		memset(seafile_program_path, 0, PATH_MAX);
	}
#endif /* __APPLE__ */

#ifdef __APPLE__
	seafile.blksize = 0;
#else
	seafile.blksize = 4096;
#endif
	/* SFTP spec says all servers should allow at least 32k I/O */
	seafile.max_read = 32768;
	seafile.max_write = 32768;
#ifdef __APPLE__
	seafile.rename_workaround = 1;
#else
	seafile.rename_workaround = 0;
#endif
	seafile.renamexdev_workaround = 0;
	seafile.truncate_workaround = 0;
	seafile.buflimit_workaround = 1;
	seafile.createmode_workaround = 0;
	seafile.ssh_ver = 2;
	seafile.progname = argv[0];
	seafile.rfd = -1;
	seafile.wfd = -1;
	seafile.ptyfd = -1;
	seafile.dir_cache = 1;
	seafile.show_help = 0;
	seafile.show_version = 0;
	seafile.singlethread = 0;
	seafile.foreground = 0;
	seafile.ptyslavefd = -1;
	seafile.delay_connect = 0;
	seafile.slave = 0;
	seafile.detect_uid = 0;
	if (strcmp(IDMAP_DEFAULT, "none") == 0) {
		seafile.idmap = IDMAP_NONE;
	} else if (strcmp(IDMAP_DEFAULT, "user") == 0) {
		seafile.idmap = IDMAP_USER;
	} else {
		fprintf(stderr, "bad idmap default value built into seafile; "
		    "assuming none (bad logic in configure script?)\n");
		seafile.idmap = IDMAP_NONE;
	}
	seafile.nomap = NOMAP_ERROR;
	ssh_add_arg("ssh");
	ssh_add_arg("-x");
	ssh_add_arg("-a");
	ssh_add_arg("-oClearAllForwardings=yes");

	if (fuse_opt_parse(&args, &seafile, seafile_opts, seafile_opt_proc) == -1 ||
	    parse_workarounds() == -1)
		exit(1);

	if (seafile.show_version) {
		printf("seafile version %s\n", PACKAGE_VERSION);
		printf("FUSE library version %s\n", fuse_pkgversion());
#if !defined(__CYGWIN__)
		fuse_lowlevel_version();
#endif
		exit(0);
	}

	if (seafile.show_help) {
		usage(args.argv[0]);
		fuse_lib_help(&args);
		exit(0);
	} else if (!seafile.host) {
		fprintf(stderr, "missing host\n");
		fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
		exit(1);
	} else if (!seafile.mountpoint) {
		fprintf(stderr, "error: no mountpoint specified\n");
		fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
		exit(1);
	}

	if (seafile.idmap == IDMAP_USER)
		seafile.detect_uid = 1;
	else if (seafile.idmap == IDMAP_FILE) {
		seafile.uid_map = NULL;
		seafile.gid_map = NULL;
		seafile.r_uid_map = NULL;
		seafile.r_gid_map = NULL;
		if (!seafile.uid_file && !seafile.gid_file) {
			fprintf(stderr, "need a uidfile or gidfile with idmap=file\n");
			exit(1);
		}
		if (seafile.uid_file)
			load_uid_map();
		if (seafile.gid_file)
			load_gid_map();
	}
	free(seafile.uid_file);
	free(seafile.gid_file);

	DEBUG("seafile version %s\n", PACKAGE_VERSION);

	/* Force seafile to the foreground when using stdin+stdout */
	if (seafile.slave)
		seafile.foreground = 1;


	if (seafile.slave && seafile.password_stdin) {
		fprintf(stderr, "the password_stdin and slave options cannot both be specified\n");
		exit(1);
	}

	if (seafile.password_stdin) {
		res = read_password();
		if (res == -1)
			exit(1);
	}

	if (seafile.debug)
		seafile.foreground = 1;
	
	if (seafile.buflimit_workaround)
		/* Work around buggy sftp-server in OpenSSH.  Without this on
		   a slow server a 10Mbyte buffer would fill up and the server
		   would abort */
		seafile.max_outstanding_len = 8388608;
	else
		seafile.max_outstanding_len = ~0;

	fsname = g_strdup(seafile.host);
	seafile.base_path = g_strdup(find_base_path());

	if (seafile.ssh_command)
		set_ssh_command();

	tmp = g_strdup_printf("-%i", seafile.ssh_ver);
	ssh_add_arg(tmp);
	g_free(tmp);
	ssh_add_arg(seafile.host);
	if (seafile.sftp_server)
		sftp_server = seafile.sftp_server;
	else if (seafile.ssh_ver == 1)
		sftp_server = SFTP_SERVER_PATH;
	else
		sftp_server = "sftp";

	if (seafile.ssh_ver != 1 && strchr(sftp_server, '/') == NULL)
		ssh_add_arg("-s");

	ssh_add_arg(sftp_server);
	free(seafile.sftp_server);

	res = cache_parse_options(&args);
	if (res == -1)
		exit(1);

	seafile.randseed = time(0);

	if (seafile.max_read > 65536)
		seafile.max_read = 65536;
	if (seafile.max_write > 65536)
		seafile.max_write = 65536;

        fsname = fsname_escape_commas(fsname);
	tmp = g_strdup_printf("-osubtype=seafile,fsname=%s", fsname);
	fuse_opt_insert_arg(&args, 1, tmp);
	g_free(tmp);
	g_free(fsname);

	if(seafile.dir_cache)
		seafile.op = cache_wrap(&seafile_oper);
	else
		seafile.op = &seafile_oper;
	fuse = fuse_new(&args, seafile.op,
			sizeof(struct fuse_operations), NULL);
	if(fuse == NULL)
		exit(1);
	se = fuse_get_session(fuse);
	res = fuse_set_signal_handlers(se);
	if (res != 0) {
		fuse_destroy(fuse);
		exit(1);
	}

	res = fuse_mount(fuse, seafile.mountpoint);
	if (res != 0) {
		fuse_destroy(fuse);
		exit(1);
	}

#if !defined(__CYGWIN__)
	res = fcntl(fuse_session_fd(se), F_SETFD, FD_CLOEXEC);
	if (res == -1)
		perror("WARNING: failed to set FD_CLOEXEC on fuse device");
#endif

	/*
	 * FIXME: trim $PATH so it doesn't contain anything inside the
	 * mountpoint, which would deadlock.
	 */
	res = ssh_connect();
	if (res == -1) {
		fuse_unmount(fuse);
		fuse_destroy(fuse);
		exit(1);
	}

	res = fuse_daemonize(seafile.foreground);
	if (res == -1) {
		fuse_unmount(fuse);
		fuse_destroy(fuse);
		exit(1);
	}

	if (seafile.singlethread)
		res = fuse_loop(fuse);
	else
		res = fuse_loop_mt(fuse, 0);

	if (res != 0)
		res = 1;
	else
		res = 0;

	fuse_remove_signal_handlers(se);
	fuse_unmount(fuse);
	fuse_destroy(fuse);

	if (seafile.debug) {
		unsigned int avg_rtt = 0;

		if (seafile.num_sent)
			avg_rtt = seafile.total_rtt / seafile.num_sent;

		DEBUG("\n"
		      "sent:               %llu messages, %llu bytes\n"
		      "received:           %llu messages, %llu bytes\n"
		      "rtt min/max/avg:    %ums/%ums/%ums\n"
		      "num connect:        %u\n",
		      (unsigned long long) seafile.num_sent,
		      (unsigned long long) seafile.bytes_sent,
		      (unsigned long long) seafile.num_received,
		      (unsigned long long) seafile.bytes_received,
		      seafile.min_rtt, seafile.max_rtt, avg_rtt,
		      seafile.num_connect);
	}

	fuse_opt_free_args(&args);
	fuse_opt_free_args(&seafile.ssh_args);
	free(seafile.directport);

	return res;
}
