/* Redis CLI (command line interface)
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>

#include "anet.h"
#include "sds.h"
#include "adlist.h"
#include "zmalloc.h"
#include "linenoise.h"

#define REDIS_CMD_INLINE 1
#define REDIS_CMD_BULK 2
#define REDIS_CMD_MULTIBULK 4

#define REDIS_NOTUSED(V) ((void) V)

static struct config {
    char *hostip;
    int hostport;
    long repeat;
    int dbnum;
    int interactive;
    int monitor_mode;
    int pubsub_mode;
    char *auth;
} config;

struct redisCommand {
    char *name;
    int arity;
};

static struct redisCommand cmdTable[] = {
    {"auth",2},
    {"get",2},
    {"set",3},
    {"setnx",3},
    {"setex",4},
    {"append",3},
    {"substr",4},
    {"del",-2},
    {"exists",2},
    {"incr",2},
    {"decr",2},
    {"rpush",3},
    {"lpush",3},
    {"rpop",2},
    {"lpop",2},
    {"brpop",-3},
    {"blpop",-3},
    {"llen",2},
    {"lindex",3},
    {"lset",4},
    {"lrange",4},
    {"ltrim",4},
    {"lrem",4},
    {"rpoplpush",3},
    {"sadd",3},
    {"srem",3},
    {"smove",4},
    {"sismember",3},
    {"scard",2},
    {"spop",2},
    {"srandmember",2},
    {"sinter",-2},
    {"sinterstore",-3},
    {"sunion",-2},
    {"sunionstore",-3},
    {"sdiff",-2},
    {"sdiffstore",-3},
    {"smembers",2},
    {"zadd",4},
    {"zincrby",4},
    {"zrem",3},
    {"zremrangebyscore",4},
    {"zmerge",-3},
    {"zmergeweighed",-4},
    {"zrange",-4},
    {"zrank",3},
    {"zrevrank",3},
    {"zrangebyscore",-4},
    {"zcount",4},
    {"zrevrange",-4},
    {"zcard",2},
    {"zscore",3},
    {"incrby",3},
    {"decrby",3},
    {"getset",3},
    {"randomkey",1},
    {"select",2},
    {"move",3},
    {"rename",3},
    {"renamenx",3},
    {"keys",2},
    {"dbsize",1},
    {"ping",1},
    {"echo",2},
    {"save",1},
    {"bgsave",1},
    {"rewriteaof",1},
    {"bgrewriteaof",1},
    {"shutdown",1},
    {"lastsave",1},
    {"type",2},
    {"flushdb",1},
    {"flushall",1},
    {"sort",-2},
    {"info",1},
    {"mget",-2},
    {"expire",3},
    {"expireat",3},
    {"ttl",2},
    {"slaveof",3},
    {"debug",-2},
    {"mset",-3},
    {"msetnx",-3},
    {"monitor",1},
    {"multi",1},
    {"exec",1},
    {"discard",1},
    {"hset",4},
    {"hget",3},
    {"hmset",-4},
    {"hmget",-3},
    {"hincrby",4},
    {"hdel",3},
    {"hlen",2},
    {"hkeys",2},
    {"hvals",2},
    {"hgetall",2},
    {"hexists",3},
    {"config",-2},
    {"subscribe",-2},
    {"unsubscribe",-1},
    {"psubscribe",-2},
    {"punsubscribe",-1},
    {"publish",3},
    {NULL,0}
};

static int cliReadReply(int fd);
static void usage();

static struct redisCommand *lookupCommand(char *name) {
    int j = 0;
    while(cmdTable[j].name != NULL) {
        if (!strcasecmp(name,cmdTable[j].name)) return &cmdTable[j];
        j++;
    }
    return NULL;
}

static int cliConnect(void) {
    char err[ANET_ERR_LEN];
    static int fd = ANET_ERR;

    if (fd == ANET_ERR) {
        fd = anetTcpConnect(err,config.hostip,config.hostport,2);
        if (fd == ANET_ERR) {
            fprintf(stderr, "Could not connect to Redis at %s:%d: %s", config.hostip, config.hostport, err);
            return -1;
        }
        anetTcpNoDelay(NULL,fd);
    }
    return fd;
}

static sds cliReadLine(int fd) {
    sds line = sdsempty();

    while(1) {
        char c;
        ssize_t ret;

        ret = read(fd,&c,1);
        if (ret == -1) {
            sdsfree(line);
            return NULL;
        } else if ((ret == 0) || (c == '\n')) {
            break;
        } else {
            line = sdscatlen(line,&c,1);
        }
    }
    return sdstrim(line,"\r\n");
}

static int cliReadSingleLineReply(int fd, int quiet) {
    sds reply = cliReadLine(fd);

    if (reply == NULL) return 1;
    if (!quiet)
        printf("%s\n", reply);
    sdsfree(reply);
    return 0;
}

static void printStringRepr(char *s, int len) {
    printf("\"");
    while(len--) {
        switch(*s) {
        case '\\':
        case '"':
            printf("\\%c",*s);
            break;
        case '\n': printf("\\n"); break;
        case '\r': printf("\\r"); break;
        case '\t': printf("\\t"); break;
        case '\a': printf("\\a"); break;
        case '\b': printf("\\b"); break;
        default:
            if (isprint(*s))
                printf("%c",*s);
            else
                printf("\\x%02x",(unsigned char)*s);
            break;
        }
        s++;
    }
    printf("\"\n");
}

static int cliReadBulkReply(int fd) {
    sds replylen = cliReadLine(fd);
    char *reply, crlf[2];
    int bulklen;

    if (replylen == NULL) return 1;
    bulklen = atoi(replylen);
    if (bulklen == -1) {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    reply = zmalloc(bulklen);
    anetRead(fd,reply,bulklen);
    anetRead(fd,crlf,2);
    if (!isatty(fileno(stdout))) {
        if (bulklen && fwrite(reply,bulklen,1,stdout) == 0) {
            zfree(reply);
            return 1;
        }
    } else {
        /* If you are producing output for the standard output we want
         * a more interesting output with quoted characters and so forth */
        printStringRepr(reply,bulklen);
    }
    zfree(reply);
    return 0;
}

static int cliReadMultiBulkReply(int fd) {
    sds replylen = cliReadLine(fd);
    int elements, c = 1;

    if (replylen == NULL) return 1;
    elements = atoi(replylen);
    if (elements == -1) {
        sdsfree(replylen);
        printf("(nil)\n");
        return 0;
    }
    if (elements == 0) {
        printf("(empty list or set)\n");
    }
    while(elements--) {
        printf("%d. ", c);
        if (cliReadReply(fd)) return 1;
        c++;
    }
    return 0;
}

static int cliReadReply(int fd) {
    char type;

    if (anetRead(fd,&type,1) <= 0) exit(1);
    switch(type) {
    case '-':
        printf("(error) ");
        cliReadSingleLineReply(fd,0);
        return 1;
    case '+':
        return cliReadSingleLineReply(fd,0);
    case ':':
        printf("(integer) ");
        return cliReadSingleLineReply(fd,0);
    case '$':
        return cliReadBulkReply(fd);
    case '*':
        return cliReadMultiBulkReply(fd);
    default:
        printf("protocol error, got '%c' as reply type byte\n", type);
        return 1;
    }
}

static int selectDb(int fd) {
    int retval;
    sds cmd;
    char type;

    if (config.dbnum == 0)
        return 0;

    cmd = sdsempty();
    cmd = sdscatprintf(cmd,"SELECT %d\r\n",config.dbnum);
    anetWrite(fd,cmd,sdslen(cmd));
    anetRead(fd,&type,1);
    if (type <= 0 || type != '+') return 1;
    retval = cliReadSingleLineReply(fd,1);
    if (retval) {
        return retval;
    }
    return 0;
}

static int cliSendCommand(int argc, char **argv, int repeat) {
    struct redisCommand *rc = lookupCommand(argv[0]);
    int fd, j, retval = 0;
    sds cmd;

    if (!rc) {
        fprintf(stderr,"Unknown command '%s'\n",argv[0]);
        return 1;
    }

    if ((rc->arity > 0 && argc != rc->arity) ||
        (rc->arity < 0 && argc < -rc->arity)) {
            fprintf(stderr,"Wrong number of arguments for '%s'\n",rc->name);
            return 1;
    }
    if (!strcasecmp(rc->name,"monitor")) config.monitor_mode = 1;
    if (!strcasecmp(rc->name,"subscribe") ||
        !strcasecmp(rc->name,"psubscribe")) config.pubsub_mode = 1;
    if ((fd = cliConnect()) == -1) return 1;

    /* Select db number */
    retval = selectDb(fd);
    if (retval) {
        fprintf(stderr,"Error setting DB num\n");
        return 1;
    }

    while(repeat--) {
        /* Build the command to send */
        cmd = sdscatprintf(sdsempty(),"*%d\r\n",argc);
        for (j = 0; j < argc; j++) {
            cmd = sdscatprintf(cmd,"$%lu\r\n",
                (unsigned long)sdslen(argv[j]));
            cmd = sdscatlen(cmd,argv[j],sdslen(argv[j]));
            cmd = sdscatlen(cmd,"\r\n",2);
        }
        anetWrite(fd,cmd,sdslen(cmd));
        sdsfree(cmd);

        while (config.monitor_mode) {
            cliReadSingleLineReply(fd,0);
        }

        if (config.pubsub_mode) {
            printf("Reading messages... (press Ctrl-c to quit)\n");
            while (1) {
                cliReadReply(fd);
                printf("\n");
            }
        }

        retval = cliReadReply(fd);
        if (retval) {
            return retval;
        }
    }
    return 0;
}

static int parseOptions(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        int lastarg = i==argc-1;

        if (!strcmp(argv[i],"-h") && !lastarg) {
            char *ip = zmalloc(32);
            if (anetResolve(NULL,argv[i+1],ip) == ANET_ERR) {
                printf("Can't resolve %s\n", argv[i]);
                exit(1);
            }
            config.hostip = ip;
            i++;
        } else if (!strcmp(argv[i],"-h") && lastarg) {
            usage();
        } else if (!strcmp(argv[i],"-p") && !lastarg) {
            config.hostport = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-r") && !lastarg) {
            config.repeat = strtoll(argv[i+1],NULL,10);
            i++;
        } else if (!strcmp(argv[i],"-n") && !lastarg) {
            config.dbnum = atoi(argv[i+1]);
            i++;
        } else if (!strcmp(argv[i],"-a") && !lastarg) {
            config.auth = argv[i+1];
            i++;
        } else if (!strcmp(argv[i],"-i")) {
            config.interactive = 1;
        } else {
            break;
        }
    }
    return i;
}

static sds readArgFromStdin(void) {
    char buf[1024];
    sds arg = sdsempty();

    while(1) {
        int nread = read(fileno(stdin),buf,1024);

        if (nread == 0) break;
        else if (nread == -1) {
            perror("Reading from standard input");
            exit(1);
        }
        arg = sdscatlen(arg,buf,nread);
    }
    return arg;
}

static void usage() {
    fprintf(stderr, "usage: redis-cli [-h host] [-p port] [-a authpw] [-r repeat_times] [-n db_num] [-i] cmd arg1 arg2 arg3 ... argN\n");
    fprintf(stderr, "usage: echo \"argN\" | redis-cli [-h host] [-a authpw] [-p port] [-r repeat_times] [-n db_num] cmd arg1 arg2 ... arg(N-1)\n");
    fprintf(stderr, "\nIf a pipe from standard input is detected this data is used as last argument.\n\n");
    fprintf(stderr, "example: cat /etc/passwd | redis-cli set my_passwd\n");
    fprintf(stderr, "example: redis-cli get my_passwd\n");
    fprintf(stderr, "example: redis-cli -r 100 lpush mylist x\n");
    fprintf(stderr, "\nRun in interactive mode: redis-cli -i or just don't pass any command\n");
    exit(1);
}

/* Turn the plain C strings into Sds strings */
static char **convertToSds(int count, char** args) {
  int j;
  char **sds = zmalloc(sizeof(char*)*count+1);

  for(j = 0; j < count; j++)
    sds[j] = sdsnew(args[j]);

  return sds;
}

static char **splitArguments(char *line, int *argc) {
    char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* skip blanks */
        while(*p && isspace(*p)) p++;
        if (*p) {
            /* get a token */
            int inq=0; /* set to 1 if we are in "quotes" */
            int done = 0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        done = 1;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            vector = zrealloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            return vector;
        }
    }
}

#define LINE_BUFLEN 4096
static void repl() {
    int argc, j;
    char *line, **argv;

    while((line = linenoise("redis> ")) != NULL) {
        if (line[0] != '\0') {
            argv = splitArguments(line,&argc);
            linenoiseHistoryAdd(line);
            if (argc > 0) {
                if (strcasecmp(argv[0],"quit") == 0 ||
                    strcasecmp(argv[0],"exit") == 0)
                        exit(0);
                else
                    cliSendCommand(argc, argv, 1);
            }
            /* Free the argument vector */
            for (j = 0; j < argc; j++)
                sdsfree(argv[j]);
            zfree(argv);
        }
        /* linenoise() returns malloc-ed lines like readline() */
        free(line);
    }
    exit(0);
}

int main(int argc, char **argv) {
    int firstarg;
    char **argvcopy;
    struct redisCommand *rc;

    config.hostip = "127.0.0.1";
    config.hostport = 6379;
    config.repeat = 1;
    config.dbnum = 0;
    config.interactive = 0;
    config.monitor_mode = 0;
    config.pubsub_mode = 0;
    config.auth = NULL;

    firstarg = parseOptions(argc,argv);
    argc -= firstarg;
    argv += firstarg;

    if (config.auth != NULL) {
        char *authargv[2];

        authargv[0] = "AUTH";
        authargv[1] = config.auth;
        cliSendCommand(2, convertToSds(2, authargv), 1);
    }

    if (argc == 0 || config.interactive == 1) repl();

    argvcopy = convertToSds(argc, argv);

    /* Read the last argument from stdandard input if needed */
    if ((rc = lookupCommand(argv[0])) != NULL) {
      if (rc->arity > 0 && argc == rc->arity-1) {
        sds lastarg = readArgFromStdin();
        argvcopy[argc] = lastarg;
        argc++;
      }
    }

    return cliSendCommand(argc, argvcopy, config.repeat);
}
