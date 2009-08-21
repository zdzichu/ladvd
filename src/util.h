
#ifndef _util_h
#define _util_h

#include <pwd.h>

#define FATAL	-1
#define CRIT	0
#define WARN	1
#define INFO	2
#define DEBUG	3

extern int8_t loglevel;

#define my_log(p, ...)	    __my_log(__func__, p, __VA_ARGS__)
#define my_fatal(...)	    __my_log(__func__, FATAL, __VA_ARGS__)
void __my_log(const char *func, int8_t prio, const char *fmt, ...);

void *my_malloc(size_t size);
void *my_calloc(size_t, size_t);
char *my_strdup(const char *str);
int my_socket(int af, int type, int proto);
void my_socketpair(int spair[2]);

size_t my_msend(int s, struct master_msg *mreq);

struct netif *netif_iter(struct netif *netif, struct nhead *);
struct netif *subif_iter(struct netif *subif, struct netif *netif);
struct netif *netif_byindex(struct nhead *, uint32_t index);
struct netif *netif_byname(struct nhead *, char *name);
void netif_protos(struct netif *netif, struct mhead *mqueue);
void netif_descr(int s, struct netif *netif, struct mhead *mqueue);

int read_line(const char *path, char *line, uint16_t len);

void my_chroot(const char *path);
void my_drop_privs(struct passwd *pwd);

uint16_t my_chksum(const void *data, size_t length, int cisco);

#endif /* _util_h */
