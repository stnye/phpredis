/* -*- Mode: C; tab-width: 4 -*- */
/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2009 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Alfonso Jimenez <yo@alfonsojimenez.com>                      |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_redis.h"
#include <zend_exceptions.h>
#define BUFFER_SIZE 1024

static int le_redis_sock;
static zend_class_entry *redis_ce;
static zend_class_entry *redis_exception_ce;
static zend_class_entry *spl_ce_RuntimeException = NULL;


zend_function_entry redis_functions[] = {
     PHP_ME(Redis, __construct, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, connect, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, close, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, ping, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, get, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, set, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, add, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, getMultiple, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, exists, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, delete, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, incr, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, decr, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, type, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, getKeys, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, getSort, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lPush, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, rPush, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lPop, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lSize, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lRemove, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, listTrim, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lGet, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lGetRange, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, lSet, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, sAdd, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, sSize, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, sRemove, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, sContains, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, sGetMembers, NULL, ZEND_ACC_PUBLIC)
     PHP_ME(Redis, setTimeout, NULL, ZEND_ACC_PUBLIC)
     PHP_MALIAS(Redis, open, connect, NULL, ZEND_ACC_PUBLIC)
     {NULL, NULL, NULL}
};

zend_module_entry redis_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
     STANDARD_MODULE_HEADER,
#endif
     "redis",
     redis_functions,
     PHP_MINIT(redis),
     PHP_MSHUTDOWN(redis),
     PHP_RINIT(redis),
     PHP_RSHUTDOWN(redis),
     PHP_MINFO(redis),
#if ZEND_MODULE_API_NO >= 20010901
     "0.1",
#endif
     STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_REDIS
ZEND_GET_MODULE(redis)
#endif


/**
 * This command behave somehow like printf, except that strings need 2 arguments:
 * Their data and their size (strlen).
 * Supported formats are: %d, %i, %s
 */
static int
redis_cmd_format(char **ret, char *format, ...) {

    char *p, *s;
    va_list ap;

    int total = 0, sz, ret_sz;
    int i;
    unsigned int u;


    int stage; /* 0: count & alloc. 1: copy. */

    for(stage = 0; stage < 2; ++stage) {
        va_start(ap, format);
        total = 0;
        for(p = format; *p; ) {

            if(*p == '%') {
                switch(*(p+1)) {
                    case 's':
                        s = va_arg(ap, char*);
                        sz = va_arg(ap, int);
                        if(stage == 1) {
                            memcpy((*ret) + total, s, sz);
                        }
                        total += sz;
                        break;

                    case 'i':
                    case 'd':
                        i = va_arg(ap, int);
                        /* compute display size of integer value */
                        sz = 1+log(abs(i))/log(10);
                        if(i == 0) { /* log 0 doesn't make sense. */
                                sz = 1;
                        } else if(i < 0) { /* allow for neg sign as well. */
                                sz++;
                        }
                        if(stage == 1) {
                            sprintf((*ret) + total, "%d", i);
                        } 
                        total += sz;
                        break;
                }
                p++;
            } else {
                if(stage == 1) {
                    (*ret)[total] = *p;
                }
                total++;
            }

            p++;
        }
        if(stage == 0) {
            ret_sz = total;
            (*ret) = emalloc(ret_sz+1);
        } else {
            (*ret)[ret_sz] = 0;
            return ret_sz;
        }
    }
}


/**
 * redis_sock_create
 */
PHPAPI RedisSock* redis_sock_create(char *host, int host_len, unsigned short port,
                                                                       long timeout)
{
    RedisSock *redis_sock;

    redis_sock         = emalloc(sizeof *redis_sock);
    redis_sock->host   = emalloc(host_len + 1);
    redis_sock->stream = NULL;
    redis_sock->status = REDIS_SOCK_STATUS_DISCONNECTED;

    memcpy(redis_sock->host, host, host_len);
    redis_sock->host[host_len] = '\0';

    redis_sock->port    = port;
    redis_sock->timeout = timeout;

    return redis_sock;
}

/**
 * redis_sock_connect
 */
PHPAPI int redis_sock_connect(RedisSock *redis_sock TSRMLS_DC)
{
    struct timeval tv;
    char *host = NULL, *hash_key = NULL, *errstr = NULL;
    int host_len, err = 0;

    if (redis_sock->stream != NULL) {
        redis_sock_disconnect(redis_sock TSRMLS_CC);
    }

    tv.tv_sec  = redis_sock->timeout;
    tv.tv_usec = 0;

    host_len = spprintf(&host, 0, "%s:%d", redis_sock->host, redis_sock->port);

    redis_sock->stream = php_stream_xport_create(host, host_len, ENFORCE_SAFE_MODE,
                                                 STREAM_XPORT_CLIENT
                                                 | STREAM_XPORT_CONNECT,
                                                 hash_key, &tv, NULL, &errstr, &err
                                                );

    efree(host);

    if (!redis_sock->stream) {
        efree(errstr);
        return -1;
    }

    php_stream_auto_cleanup(redis_sock->stream);

    php_stream_set_option(redis_sock->stream, 
                          PHP_STREAM_OPTION_READ_TIMEOUT,
                          0, &tv);
    php_stream_set_option(redis_sock->stream,
                          PHP_STREAM_OPTION_WRITE_BUFFER,
                          PHP_STREAM_BUFFER_NONE, NULL);

    redis_sock->status = REDIS_SOCK_STATUS_CONNECTED;

    return 0;
}

/**
 * redis_sock_server_open
 */
PHPAPI int redis_sock_server_open(RedisSock *redis_sock, int force_connect TSRMLS_DC)
{
    int res = -1;

    switch (redis_sock->status) {
        case REDIS_SOCK_STATUS_DISCONNECTED:
            return redis_sock_connect(redis_sock TSRMLS_CC);
        case REDIS_SOCK_STATUS_CONNECTED:
            res = 0;
        break;
        case REDIS_SOCK_STATUS_UNKNOWN:
            if (force_connect > 0 && redis_sock_connect(redis_sock TSRMLS_CC) < 0) {
                res = -1;
            } else {
                res = 0;

                redis_sock->status = REDIS_SOCK_STATUS_CONNECTED;
            }
        break;
    }

    return res;
}

/**
 * redis_sock_disconnect
 */
PHPAPI int redis_sock_disconnect(RedisSock *redis_sock TSRMLS_DC)
{
    int res = 0;

    if (redis_sock->stream != NULL) {
        redis_sock_write(redis_sock, "QUIT", sizeof("QUIT") - 1);

        redis_sock->status = REDIS_SOCK_STATUS_DISCONNECTED;
        php_stream_close(redis_sock->stream);
        redis_sock->stream = NULL;

        res = 1;
    }

    return res;
}

/**
 * redis_sock_read
 */
PHPAPI char *redis_sock_read(RedisSock *redis_sock, int *buf_len TSRMLS_DC)
{

    char inbuf[1024];
    char *resp;

    php_stream_gets(redis_sock->stream, inbuf, 1024);


    switch(inbuf[0]) {

        case '-':
        case '+':
        case ':':    
            // Single Line Reply 
            /* :123\r\n */
            *buf_len = strlen(inbuf) - 2;
            if(*buf_len >= 2) {
                resp = emalloc(1+*buf_len);
                memcpy(resp, inbuf, *buf_len);
                resp[*buf_len] = 0;
                return resp;
            } else {
                printf("protocol error \n");
                return NULL;
            }

        case '$':
            *buf_len = atoi(inbuf + 1);
            resp = redis_sock_read_bulk_reply(redis_sock, *buf_len);
            return resp;

        default:
            printf("protocol error, got '%c' as reply type byte\n", inbuf[0]);
    }

    return NULL;
}

/**
 * redis_sock_read_bulk_reply
 */
PHPAPI char *redis_sock_read_bulk_reply(RedisSock *redis_sock, int bytes)
{

    int offset = 0;
    size_t got;

    char * reply;

    if (bytes == -1) {
	  return NULL;
    } else {

        reply = emalloc(bytes+1);

        while(offset < bytes) {
            got = php_stream_read(redis_sock->stream, reply + offset, bytes-offset);
            offset += got;
        }
        char c;
        int i;
        for(i = 0; i < 2; i++) {
            php_stream_read(redis_sock->stream, &c, 1);
        }
    }

    reply[bytes] = 0;
    return reply;
}

/**
 * redis_sock_read_multibulk_reply
 */
PHPAPI int redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAMETERS,
                                      RedisSock *redis_sock, int *buf_len TSRMLS_DC)
{
    char inbuf[1024], *response;
    int response_len;

    php_stream_gets(redis_sock->stream, inbuf, 1024);

    if(inbuf[0] != '*') {
        return -1;
    }
    int numElems = atoi(inbuf+1);

    array_init(return_value);

    while(numElems > 0) {
        int response_len;
        response = redis_sock_read(redis_sock, &response_len TSRMLS_CC);

        if(response != NULL) {
            add_next_index_stringl(return_value, response, response_len, 0);
        } else {
            add_next_index_bool(return_value, 0);
        }
        numElems --;
    }
    return 0;
}

/**
 * redis_sock_write
 */
PHPAPI int redis_sock_write(RedisSock *redis_sock, char *cmd, size_t sz)
{
    php_stream_write(redis_sock->stream, cmd, sz);
    return 0;
}

/**
 * redis_sock_get
 */
PHPAPI int redis_sock_get(zval *id, RedisSock **redis_sock TSRMLS_DC)
{

    zval **socket;
    int resource_type;

    if (Z_TYPE_P(id) != IS_OBJECT || zend_hash_find(Z_OBJPROP_P(id), "socket",
                                  sizeof("socket"), (void **) &socket) == FAILURE) {
        return -1;
    }

    *redis_sock = (RedisSock *) zend_list_find(Z_LVAL_PP(socket), &resource_type);

    if (!*redis_sock || resource_type != le_redis_sock) {
            return -1;
    }

    return Z_LVAL_PP(socket);
}

/**
 * redis_free_socket
 */
PHPAPI void redis_free_socket(RedisSock *redis_sock)
{
    efree(redis_sock->host);
    efree(redis_sock);
}

PHPAPI zend_class_entry *redis_get_exception_base(int root TSRMLS_DC)
{
#if HAVE_SPL
        if (!root) {
                if (!spl_ce_RuntimeException) {
                        zend_class_entry **pce;
        
                        if (zend_hash_find(CG(class_table), "runtimeexception",
                                                           sizeof("RuntimeException"), (void **) &pce) == SUCCESS) {
                                spl_ce_RuntimeException = *pce;
                                return *pce;
                        }
                } else {
                        return spl_ce_RuntimeException;
                }
        }
#endif
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION < 2)
        return zend_exception_get_default();
#else
        return zend_exception_get_default(TSRMLS_C);
#endif
}

/**
 * redis_destructor_redis_sock
 */
static void redis_destructor_redis_sock(zend_rsrc_list_entry * rsrc TSRMLS_DC)
{
    RedisSock *redis_sock = (RedisSock *) rsrc->ptr;
    redis_sock_disconnect(redis_sock TSRMLS_CC);
    redis_free_socket(redis_sock);
}

/**
 * PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(redis)
{
    zend_class_entry redis_class_entry;
    INIT_CLASS_ENTRY(redis_class_entry, "Redis", redis_functions);
    redis_ce = zend_register_internal_class(&redis_class_entry TSRMLS_CC);

    zend_class_entry redis_exception_class_entry;
    INIT_CLASS_ENTRY(redis_exception_class_entry, "RedisException", NULL);
    redis_exception_ce = zend_register_internal_class_ex(
        &redis_exception_class_entry,
        redis_get_exception_base(0 TSRMLS_CC),
        NULL TSRMLS_CC
    );

    le_redis_sock = zend_register_list_destructors_ex(
        redis_destructor_redis_sock,
        NULL,
        redis_sock_name, module_number
    );

    return SUCCESS;
}

/**
 * PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(redis)
{
    return SUCCESS;
}

/**
 * PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(redis)
{
    return SUCCESS;
}

/**
 * PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(redis)
{
    return SUCCESS;
}

/**
 * PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(redis)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "Redis Support", "enabled");
    php_info_print_table_row(2, "Version", PHP_REDIS_VERSION);
    php_info_print_table_end();
}

/* {{{ proto Redis Redis::__construct()
    Public constructor */
PHP_METHOD(Redis, __construct)
{
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "") == FAILURE) {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::connect(string host, int port [, int timeout])
 */
PHP_METHOD(Redis, connect)
{
    zval *object;
    int host_len, id;
    char *host = NULL;
    long port;

    struct timeval timeout = {5L, 0L};
    RedisSock *redis_sock  = NULL;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Osl|l",
                                     &object, redis_ce, &host, &host_len, &port,
                                     &timeout.tv_sec) == FAILURE) {
       RETURN_FALSE;
    }

    if (timeout.tv_sec < 0L || timeout.tv_sec > INT_MAX) {
        zend_throw_exception(redis_exception_ce, "Invalid timeout", 0 TSRMLS_CC);
        RETURN_FALSE;
    }

    redis_sock = redis_sock_create(host, host_len, port, timeout.tv_sec);

    if (redis_sock_server_open(redis_sock, 1 TSRMLS_CC) < 0) {
        redis_free_socket(redis_sock);
        zend_throw_exception_ex(
            redis_exception_ce,
            0 TSRMLS_CC,
            "Can't connect to %s:%d",
            host,
            port
        );
        RETURN_FALSE;
    }

    id = zend_list_insert(redis_sock, le_redis_sock);
    add_property_resource(object, "socket", id);

    RETURN_TRUE;
}
/* }}} */

/* {{{ proto boolean Redis::close()
 */
PHP_METHOD(Redis, close)
{
    zval *object;
    RedisSock *redis_sock = NULL;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O",
        &object, redis_ce) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    if (redis_sock_disconnect(redis_sock TSRMLS_CC)) {
        RETURN_TRUE;
    }

    RETURN_FALSE;
}
/* }}} */

/* {{{ proto boolean Redis::set(string key, string value)
 */
PHP_METHOD(Redis, set)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *val = NULL, *cmd, *response;
    int key_len, val_len, cmd_len, response_len, count;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce, &key, &key_len,
                                     &val, &val_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "SET %s %d\r\n%s\r\n", key, key_len, val_len, val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);
    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[0] == 0x2b) {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto string Redis::get(string key)
 */
PHP_METHOD(Redis, get)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "GET %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if(strncmp(response, "-ERR", 4) == 0) {
        efree(response);
        RETURN_FALSE;
    }
    RETURN_STRINGL(response, response_len, 0);
}
/* }}} */

/* {{{ proto boolean Redis::add(string key, string value)
 */
PHP_METHOD(Redis, add)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *val = NULL, *cmd, *response;
    int key_len, val_len, response_len;
    int cmd_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce, &key, &key_len,
                                     &val, &val_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "SETNX %s %d\r\n%s\r\n",
                    key, key_len,
                    val_len,
                    val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[1] == 0x31) {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto string Redis::ping()
 */
PHP_METHOD(Redis, ping)
{
    zval *object;
    RedisSock *redis_sock;
    char *cmd, *response;
    int cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "O",
                                     &object, redis_ce) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    if (redis_sock->stream) {
       char cmd[] = "PING\r\n";
       char buf[8];

       redis_sock_write(redis_sock, cmd, sizeof(cmd)-1);

       response = redis_sock_read(redis_sock, &response_len TSRMLS_CC);

       RETURN_STRING(response, 1);
    } else {
       php_error_docref(NULL TSRMLS_CC, E_ERROR, "The object is not connected");
       RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::incr(string key [,int value])
 */
PHP_METHOD(Redis, incr)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;
    long val = 0;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os|l",
                                     &object, redis_ce,
                                     &key, &key_len, &val) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    if (val <= 1) {
        cmd_len = spprintf(&cmd, 0, "INCR %s\r\n", key);
    } else {
        cmd_len = spprintf(&cmd, 0, "INCRBY %s %d\r\n", key, (int)val);
    }

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[0] == ':') {
        long ret = atoi(response+1);
        efree(response);
        RETURN_LONG(ret);
    } else {
        efree(response);
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::decr(string key [,int value])
 */
PHP_METHOD(Redis, decr)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len,cmd_len, response_len;
    long val = 0;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os|l",
                                     &object, redis_ce,
                                     &key, &key_len, &val) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    if (val <= 1) {
        cmd_len = spprintf(&cmd, 0, "DECR %s\r\n", key);
    } else {
        cmd_len = spprintf(&cmd, 0, "DECRBY %s %d\r\n", key, (int)val);
    }

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[0] == ':') {
        long ret = atoi(response+1);
        efree(response);
        RETURN_LONG(ret);
    } else {
        efree(response);
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::getMultiple(array keys)
 */
PHP_METHOD(Redis, getMultiple)
{
    zval *object, *array, **data;
    HashTable *arr_hash;
    HashPosition pointer;
    RedisSock *redis_sock;
    char *cmd = "", *response;
    int cmd_len, response_len, array_count;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oa",
                                     &object, redis_ce, &array) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    arr_hash    = Z_ARRVAL_P(array);
    array_count = zend_hash_num_elements(arr_hash);

    if (array_count == 0) {
        RETURN_FALSE;
    }

    for (zend_hash_internal_pointer_reset_ex(arr_hash, &pointer);
         zend_hash_get_current_data_ex(arr_hash, (void**) &data,
                                       &pointer) == SUCCESS;
         zend_hash_move_forward_ex(arr_hash, &pointer)) {

        if (Z_TYPE_PP(data) == IS_STRING) {
            cmd_len = spprintf(&cmd, 0, "%s %s", cmd, Z_STRVAL_PP(data));
        }
    }

    cmd_len = spprintf(&cmd, 0, "MGET%s\r\n", cmd);
    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if (redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                                        redis_sock, &response_len TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::exists(string key)
 */
PHP_METHOD(Redis, exists)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "EXISTS %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }

    efree(cmd);
    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    char c = (response && response_len > 1 ? response[1] : 0); /* character at [1] if available, otherwise NIL */
    if(response) {
        efree(response);
    }

    if (c == '1') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::delete(string key)
 */
PHP_METHOD(Redis, delete)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "DEL %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }

    efree(cmd);
    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    char c = (response && response_len > 1 ? response[1] : 0); /* character at [1] if available, otherwise NIL */
    if(response) {
        efree(response);
    }

    if (c == '1') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::getKeys(string pattern)
 */
PHP_METHOD(Redis, getKeys)
{
    zval *object;
    RedisSock *redis_sock;
    char *pattern = NULL, *cmd, *response;
    int pattern_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &pattern, &pattern_len) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "KEYS %s\r\n", pattern);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    /* prepare for php_explode */
    array_init(return_value);

    if(response_len) { /* empty array in case of an empty string */
        zval *delimiter; /* delimiter */
        MAKE_STD_ZVAL(delimiter);
        ZVAL_STRING(delimiter, " ", 1);

        zval *keys; /* keys */
        MAKE_STD_ZVAL(keys);
        ZVAL_STRING(keys, response, 1);
        php_explode(delimiter, keys, return_value, -1);

        /* free memory */
        zval_dtor(keys);
        efree(keys);
        zval_dtor(delimiter);
        efree(delimiter);
    }

    efree(response);
}
/* }}} */

/* {{{ proto int Redis::type(string key)
 */
PHP_METHOD(Redis, type)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "TYPE %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    long l;
    if (strncmp(response, "+string", 7) == 0) {
       l = 1;
    } else if (strncmp(response, "+set", 4) == 0){
       l = 2;
    } else if (strncmp(response, "+list", 5) == 0){
       l = 3;
    } else {
       l = 0;
    }
    efree(response);
    RETURN_LONG(l);
}
/* }}} */

/* {{{ proto boolean Redis::lPush(string key , string value)
 */
PHP_METHOD(Redis, lPush)
{
    zval *object;
    RedisSock *redis_sock;
    char *cmd, *response, *key, *val;
    int cmd_len, response_len, key_len, val_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce,
                                     &key, &key_len, &val, &val_len) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "LPUSH %s %d\r\n%s\r\n",
                               key, key_len,
                               val_len,
                               val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    char c = 0;
    if(response) {
        c = response[0];
        efree(response);
    }
    if (c == '+') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}

/* {{{ proto boolean Redis::rPush(string key , string value)
 */
PHP_METHOD(Redis, rPush)
{
    zval *object;
    RedisSock *redis_sock;
    char *cmd, *response, *key, *val;
    int cmd_len, response_len, key_len, val_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce,
                                     &key, &key_len, &val, &val_len) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "RPUSH %s %d\r\n%s\r\n",
                               key, key_len,
                               val_len,
                               val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    char c = 0;
    if(response) {
        c = response[0];
        efree(response);
    }
    if (c == '+') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto string Redis::lPOP(string key , [, int type=0])
 */
PHP_METHOD(Redis, lPop)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;
    long type = 0;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os|l",
                                     &object, redis_ce,
                                     &key, &key_len, &type) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    if (type == 0) {
        cmd_len = spprintf(&cmd, 0, "RPOP %s\r\n", key);
    } else if (type == 1) {
        cmd_len = spprintf(&cmd, 0, "LPOP %s\r\n", key);
    } else {
        RETURN_FALSE;
    }

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (strncmp(response, "nil", 4) != 0) {
        RETURN_STRINGL(response, response_len, 0);
    } else {
        efree(response);
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto int Redis::lSize(string key)
 */
PHP_METHOD(Redis, lSize)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "LLEN %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if(response) {
        if(response[0] == ':') {
            int res = atoi(response + 1);
            efree(response);
            RETURN_LONG(res);
        }
        efree(response);
        RETURN_FALSE;
    }

}
/* }}} */

/* {{{ proto boolean Redis::lRemove(string list, string value, int count = 0)
 */
PHP_METHOD(Redis, lRemove)
{
    zval *object;
    RedisSock *redis_sock;
    char *cmd, *response;
    int cmd_len, response_len, key_len, val_len;
    char *key, *val;
    long count = 0;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss|l",
                                     &object, redis_ce,
                                     &key, &key_len, &val, &val_len, &count) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "LREM %s %d %d\r\n%s\r\n",
                       key, key_len,
                       count, 
                       val_len,
                       val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if(response[0] == ':') {
        long l = atoi(response + 1);
        efree(response);
        RETURN_LONG(l);
    } else {
        efree(response);
        RETURN_FALSE;
    }

}
/* }}} */

/* {{{ proto boolean Redis::listTrim(string key , int start , int end)
 */
PHP_METHOD(Redis, listTrim)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *val = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;
    long start, end;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Osll",
                                     &object, redis_ce, &key, &key_len,
                                     &start, &end) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "LTRIM %s %d %d\r\n", key, (int)start, (int)end);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    char c = response[0];
    efree(response);
    if (c == '+') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto string Redis::lGet(string key , int index)
 */
PHP_METHOD(Redis, lGet)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len,cmd_len, response_len;
    long index;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Osl",
                                     &object, redis_ce,
                                     &key, &key_len, &index) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "LINDEX %s %d\r\n", key, (int)index);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (strncmp(response, "nil", 4) != 0) {
        RETURN_STRINGL(response, response_len, 0);
    } else {
        efree(response);
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::lGetRange(string key, int start , int end)
 */
PHP_METHOD(Redis, lGetRange)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;
    long start, end;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Osll",
                                     &object, redis_ce,
                                     &key, &key_len, &start, &end) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "LRANGE %s %d %d\r\n\r\n", key, (int)start, (int)end);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if (redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                                        redis_sock, &response_len TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::sAdd(string key , string value)
 */
PHP_METHOD(Redis, sAdd)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *val = NULL, *cmd, *response;
    int key_len, val_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce, &key, &key_len,
                                     &val, &val_len) == FAILURE) {
        RETURN_NULL();
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "SADD %s %d\r\n%s\r\n",
                    key, key_len,
                    val_len, 
                    val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[1] == 0x31) {
        RETURN_TRUE;
    } else if (response[1] == '0') {
        php_error_docref(NULL TSRMLS_CC, E_WARNING,
                         "The new element was already a member of the set");
        RETURN_FALSE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto int Redis::sSize(string key)
 */
PHP_METHOD(Redis, sSize)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "SCARD %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    int res = atoi(response + 1);

    RETURN_LONG(res);
}
/* }}} */

/* {{{ proto boolean Redis::sRemove(string set, string value)
 */
PHP_METHOD(Redis, sRemove)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *val = NULL, *cmd, *response;
    int key_len, val_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce,
                                     &key, &key_len, &val, &val_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "SREM %s %d\r\n%s\r\n",
                               key, key_len,
                               val_len, 
                               val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[1] == '1') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto boolean Redis::sContains(string set, string value)
 */
PHP_METHOD(Redis, sContains)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *val = NULL, *cmd, *response;
    int key_len, val_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Oss",
                                     &object, redis_ce,
                                     &key, &key_len, &val, &val_len) == FAILURE) {
        return;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "SISMEMBER %s %d\r\n%s\r\n",
                               key, key_len,
                               val_len, 
                               val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {
        RETURN_FALSE;
    }

    if (response[1] == '1') {
        RETURN_TRUE;
    } else {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::sGetMembers(string set)
 */
PHP_METHOD(Redis, sGetMembers)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, response_len;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os",
                                     &object, redis_ce,
                                     &key, &key_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "SMEMBERS %s\r\n", key);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        RETURN_FALSE;
    }

    if (redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                                        redis_sock, &response_len TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::getSort(string key [,order = 0, pattern = "*", start=0,
 *                                                                       end = 0])
 */
PHP_METHOD(Redis, getSort)
{
    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *pattern = "*", *cmd, *response, *limit = "";
    char order_str[] = "DESC";
    int key_len, pattern_len, cmd_len, response_len, limit_len, order_len;
    zend_bool alpha = 0;
    long start = 0, end = 0, order = 0;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Os|lsll",
                                     &object, redis_ce,
                                     &key, &key_len, &order, &pattern, &pattern_len,
                                     &start, &end) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    if (start != 0 && end != 0) {
        limit_len = spprintf(&limit, 0, "LIMIT %d %d", (int)start, (int)end);
    }

    if (order == 1) {
        strcpy(order_str, "ASC");
    }
    
    cmd_len = spprintf(&cmd, 0, "SORT %s BY %s %s %s ALPHA\r\n", key, pattern,
                       limit, order_str);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        efree(limit);
        RETURN_FALSE;
    }
    efree(cmd);
    efree(limit);

    if (redis_sock_read_multibulk_reply(INTERNAL_FUNCTION_PARAM_PASSTHRU,
                                        redis_sock, &response_len TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::setTimeout(string key, int timeout)
 */
PHP_METHOD(Redis, setTimeout) {

    zval *object;
    RedisSock *redis_sock;
    char *key = NULL, *cmd, *response;
    int key_len, cmd_len, count, response_len;
    long timeout;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Osl",
                                     &object, redis_ce, &key, &key_len,
                                     &timeout) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = spprintf(&cmd, 0, "EXPIRE %s %d\r\n", key, (int)timeout);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {        
        RETURN_FALSE;
    }
    char c = response_len > 1 ? response[1] : 0;
    efree(response);

    if (c == '1') {
        RETURN_TRUE;
    } else {    
        RETURN_FALSE;
    }
}
/* }}} */

/* {{{ proto array Redis::lSet(string key, int index, string value)
 */
PHP_METHOD(Redis, lSet) {

    zval *object;
    RedisSock *redis_sock;

    char *cmd, *response;
    int cmd_len, response_len, key_len, val_len;
    long index;
    char *key, *val;

    if (zend_parse_method_parameters(ZEND_NUM_ARGS() TSRMLS_CC, getThis(), "Osls",
                                     &object, redis_ce, &key, &key_len, &index, &val, &val_len) == FAILURE) {
        RETURN_FALSE;
    }

    if (redis_sock_get(object, &redis_sock TSRMLS_CC) < 0) {
        RETURN_FALSE;
    }

    cmd_len = redis_cmd_format(&cmd, "LSET %s %d %d\r\n%s\r\n",
                               key, key_len,
                               (int)index,
                               val_len,
                               val, val_len);

    if (redis_sock_write(redis_sock, cmd, cmd_len) < 0) {
        efree(cmd);
        RETURN_FALSE;
    }
    efree(cmd);

    if ((response = redis_sock_read(redis_sock, &response_len TSRMLS_CC)) == NULL) {        
        RETURN_FALSE;
    }

    if (strncmp(response, "+OK", 4) == 0) {
        efree(response);
        RETURN_TRUE;
    } else {    
        efree(response);
        RETURN_FALSE;
    }
}
/* vim: set tabstop=4 expandtab: */
/* }}} */