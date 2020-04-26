/*
 * if_alg: User-space algorithm interface
 *
 * Copyright (c) 2010 Herbert Xu <herbert@gondor.apana.org.au>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#ifndef _CRYPTO_IF_ALG_H
#define _CRYPTO_IF_ALG_H

#include <linux/compiler.h>
#include <linux/completion.h>
#include <linux/if_alg.h>
#include <linux/scatterlist.h>
#include <linux/types.h>
#include <net/sock.h>

#include <crypto/aead.h>
#include <crypto/skcipher.h>

#define ALG_MAX_PAGES			16

struct crypto_async_request;

struct alg_sock {
	/* struct sock must be the first member of struct alg_sock */
	struct sock sk;

	struct sock *parent;

	unsigned int refcnt;
	unsigned int nokey_refcnt;

	const struct af_alg_type *type;
	void *private;
};

struct af_alg_completion {
	struct completion completion;
	int err;
};

struct af_alg_control {
	struct af_alg_iv *iv;
	int op;
	unsigned int aead_assoclen;
};

struct af_alg_type {
	void *(*bind)(const char *name, u32 type, u32 mask);
	void (*release)(void *private);
	int (*setkey)(void *private, const u8 *key, unsigned int keylen);
	int (*accept)(void *private, struct sock *sk);
	int (*accept_nokey)(void *private, struct sock *sk);
	int (*setauthsize)(void *private, unsigned int authsize);

	struct proto_ops *ops;
	struct proto_ops *ops_nokey;
	struct module *owner;
	char name[14];
};

struct af_alg_sgl {
	struct scatterlist sg[ALG_MAX_PAGES + 1];
	struct page *pages[ALG_MAX_PAGES];
	unsigned int npages;
};

/* TX SGL entry */
struct af_alg_tsgl {
	struct list_head list;
	unsigned int cur;		/* Last processed SG entry */
	struct scatterlist sg[0];	/* Array of SGs forming the SGL */
};

#define MAX_SGL_ENTS ((4096 - sizeof(struct af_alg_tsgl)) / \
		      sizeof(struct scatterlist) - 1)

/* RX SGL entry */
struct af_alg_rsgl {
	struct af_alg_sgl sgl;
	struct list_head list;
	size_t sg_num_bytes;		/* Bytes of data in that SGL */
};

/**
 * struct af_alg_async_req - definition of crypto request
 * @iocb:		IOCB for AIO operations
 * @sk:			Socket the request is associated with
 * @first_rsgl:		First RX SG
 * @last_rsgl:		Pointer to last RX SG
 * @rsgl_list:		Track RX SGs
 * @tsgl:		Private, per request TX SGL of buffers to process
 * @tsgl_entries:	Number of entries in priv. TX SGL
 * @outlen:		Number of output bytes generated by crypto op
 * @areqlen:		Length of this data structure
 * @cra_u:		Cipher request
 */
struct af_alg_async_req {
	struct kiocb *iocb;
	struct sock *sk;

	struct af_alg_rsgl first_rsgl;
	struct af_alg_rsgl *last_rsgl;
	struct list_head rsgl_list;

	struct scatterlist *tsgl;
	unsigned int tsgl_entries;

	unsigned int outlen;
	unsigned int areqlen;

	union {
		struct aead_request aead_req;
		struct skcipher_request skcipher_req;
	} cra_u;

	/* req ctx trails this struct */
};

/**
 * struct af_alg_ctx - definition of the crypto context
 *
 * The crypto context tracks the input data during the lifetime of an AF_ALG
 * socket.
 *
 * @tsgl_list:		Link to TX SGL
 * @iv:			IV for cipher operation
 * @aead_assoclen:	Length of AAD for AEAD cipher operations
 * @completion:		Work queue for synchronous operation
 * @used:		TX bytes sent to kernel. This variable is used to
 *			ensure that user space cannot cause the kernel
 *			to allocate too much memory in sendmsg operation.
 * @rcvused:		Total RX bytes to be filled by kernel. This variable
 *			is used to ensure user space cannot cause the kernel
 *			to allocate too much memory in a recvmsg operation.
 * @more:		More data to be expected from user space?
 * @merge:		Shall new data from user space be merged into existing
 *			SG?
 * @enc:		Cryptographic operation to be performed when
 *			recvmsg is invoked.
 * @len:		Length of memory allocated for this data structure.
 */
struct af_alg_ctx {
	struct list_head tsgl_list;

	void *iv;
	size_t aead_assoclen;

	struct af_alg_completion completion;

	size_t used;
	size_t rcvused;

	bool more;
	bool merge;
	bool enc;

	unsigned int len;
};

int af_alg_register_type(const struct af_alg_type *type);
int af_alg_unregister_type(const struct af_alg_type *type);

int af_alg_release(struct socket *sock);
void af_alg_release_parent(struct sock *sk);
int af_alg_accept(struct sock *sk, struct socket *newsock, bool kern);

int af_alg_make_sg(struct af_alg_sgl *sgl, struct iov_iter *iter, int len);
void af_alg_free_sg(struct af_alg_sgl *sgl);
void af_alg_link_sg(struct af_alg_sgl *sgl_prev, struct af_alg_sgl *sgl_new);

int af_alg_cmsg_send(struct msghdr *msg, struct af_alg_control *con);

int af_alg_wait_for_completion(int err, struct af_alg_completion *completion);
void af_alg_complete(struct crypto_async_request *req, int err);

static inline struct alg_sock *alg_sk(struct sock *sk)
{
	return (struct alg_sock *)sk;
}

static inline void af_alg_init_completion(struct af_alg_completion *completion)
{
	init_completion(&completion->completion);
}

/**
 * Size of available buffer for sending data from user space to kernel.
 *
 * @sk socket of connection to user space
 * @return number of bytes still available
 */
static inline int af_alg_sndbuf(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;

	return max_t(int, max_t(int, sk->sk_sndbuf & PAGE_MASK, PAGE_SIZE) -
			  ctx->used, 0);
}

/**
 * Can the send buffer still be written to?
 *
 * @sk socket of connection to user space
 * @return true => writable, false => not writable
 */
static inline bool af_alg_writable(struct sock *sk)
{
	return PAGE_SIZE <= af_alg_sndbuf(sk);
}

/**
 * Size of available buffer used by kernel for the RX user space operation.
 *
 * @sk socket of connection to user space
 * @return number of bytes still available
 */
static inline int af_alg_rcvbuf(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct af_alg_ctx *ctx = ask->private;

	return max_t(int, max_t(int, sk->sk_rcvbuf & PAGE_MASK, PAGE_SIZE) -
			  ctx->rcvused, 0);
}

/**
 * Can the RX buffer still be written to?
 *
 * @sk socket of connection to user space
 * @return true => writable, false => not writable
 */
static inline bool af_alg_readable(struct sock *sk)
{
	return PAGE_SIZE <= af_alg_rcvbuf(sk);
}

int af_alg_alloc_tsgl(struct sock *sk);
unsigned int af_alg_count_tsgl(struct sock *sk, size_t bytes, size_t offset);
void af_alg_pull_tsgl(struct sock *sk, size_t used, struct scatterlist *dst,
		      size_t dst_offset);
void af_alg_free_areq_sgls(struct af_alg_async_req *areq);
int af_alg_wait_for_wmem(struct sock *sk, unsigned int flags);
void af_alg_wmem_wakeup(struct sock *sk);
int af_alg_wait_for_data(struct sock *sk, unsigned flags);
void af_alg_data_wakeup(struct sock *sk);
int af_alg_sendmsg(struct socket *sock, struct msghdr *msg, size_t size,
		   unsigned int ivsize);
ssize_t af_alg_sendpage(struct socket *sock, struct page *page,
			int offset, size_t size, int flags);
void af_alg_async_cb(struct crypto_async_request *_req, int err);
unsigned int af_alg_poll(struct file *file, struct socket *sock,
			 poll_table *wait);
struct af_alg_async_req *af_alg_alloc_areq(struct sock *sk,
					   unsigned int areqlen);
int af_alg_get_rsgl(struct sock *sk, struct msghdr *msg, int flags,
		    struct af_alg_async_req *areq, size_t maxsize,
		    size_t *outlen);

#endif	/* _CRYPTO_IF_ALG_H */
