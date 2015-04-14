/*	$OpenBSD: bcrypt.c,v 1.52 2015/01/28 23:33:52 tedu Exp $	*/

/*
 * Copyright (c) 2014 Ted Unangst <tedu@openbsd.org>
 * Copyright (c) 1997 Niels Provos <provos@umich.edu>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/* This password hashing algorithm was designed by David Mazieres
 * <dm@lcs.mit.edu> and works as follows:
 *
 * 1. state := InitState ()
 * 2. state := ExpandKey (state, salt, password)
 * 3. REPEAT rounds:
 *      state := ExpandKey (state, 0, password)
 *	state := ExpandKey (state, 0, salt)
 * 4. ctext := "OrpheanBeholderScryDoubt"
 * 5. REPEAT 64:
 * 	ctext := Encrypt_ECB (state, ctext);
 * 6. RETURN Concatenate (salt, ctext);
 *
 */
/* The scheduler-friendly Erlang NIF version of this password hashing
 * algorithm was implemented by Jason M Barnes.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <sys/time.h>
#include <pwd.h>

#include "erl_nif.h"
#include "erl_blf.h"

/* This implementation is adaptable to current computing power.
 * You can have up to 2^31 rounds which should be enough for some
 * time to come.
 */

#define BCRYPT_VERSION '2'
#define BCRYPT_MAXSALT 16	/* Precomputation is just so nice */
#define BCRYPT_WORDS 6		/* Ciphertext words */
#define BCRYPT_MINLOGROUNDS 4	/* we have log2(rounds) in salt */

#define	BCRYPT_SALTSPACE	(7 + (BCRYPT_MAXSALT * 4 + 2) / 3 + 1)
#define	BCRYPT_HASHSPACE	61

static int encode_base64(char *, const u_int8_t *, size_t);
static int decode_base64(u_int8_t *, size_t, const char *);
static ERL_NIF_TERM bcrypt_expandstate(ErlNifEnv *, int, const ERL_NIF_TERM *);
static ERL_NIF_TERM bcrypt_fini(ErlNifEnv *, int, const ERL_NIF_TERM *);
static void secure_bzero(void *, size_t);
static int calc_percent(int *, struct timeval *, struct timeval *);
static unsigned long adjust_max_per_slice(int, int, u_int32_t, unsigned long);

static const u_int8_t Base64Code[] =
"./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

static const u_int8_t index_64[128] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 0, 1, 54, 55,
	56, 57, 58, 59, 60, 61, 62, 63, 255, 255,
	255, 255, 255, 255, 255, 2, 3, 4, 5, 6,
	7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
	255, 255, 255, 255, 255, 255, 28, 29, 30,
	31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
	51, 52, 53, 255, 255, 255, 255, 255
};
#define CHAR64(c)  ( (c) > 127 ? 255 : index_64[(c)])

/*
 * read buflen (after decoding) bytes of data from b64data
 */
static int
decode_base64(u_int8_t *buffer, size_t len, const char *b64data)
{
	u_int8_t *bp = buffer;
	const u_int8_t *p = (const u_int8_t *)b64data;
	u_int8_t c1, c2, c3, c4;

	while (bp < buffer + len) {
		c1 = CHAR64(*p);
		/* Invalid data */
		if (c1 == 255)
			return -1;

		c2 = CHAR64(*(p + 1));
		if (c2 == 255)
			return -1;

		*bp++ = (c1 << 2) | ((c2 & 0x30) >> 4);
		if (bp >= buffer + len)
			break;

		c3 = CHAR64(*(p + 2));
		if (c3 == 255)
			return -1;

		*bp++ = ((c2 & 0x0f) << 4) | ((c3 & 0x3c) >> 2);
		if (bp >= buffer + len)
			break;

		c4 = CHAR64(*(p + 3));
		if (c4 == 255)
			return -1;
		*bp++ = ((c3 & 0x03) << 6) | c4;

		p += 4;
	}
	return 0;
}

/*
 * Turn len bytes of data into base64 encoded data.
 * This works without = padding.
 */
static int
encode_base64(char *b64buffer, const u_int8_t *data, size_t len)
{
	u_int8_t *bp = (u_int8_t *)b64buffer;
	const u_int8_t *p = data;
	u_int8_t c1, c2;

	while (p < data + len) {
		c1 = *p++;
		*bp++ = Base64Code[(c1 >> 2)];
		c1 = (c1 & 0x03) << 4;
		if (p >= data + len) {
			*bp++ = Base64Code[c1];
			break;
		}
		c2 = *p++;
		c1 |= (c2 >> 4) & 0x0f;
		*bp++ = Base64Code[c1];
		c1 = (c2 & 0x0f) << 2;
		if (p >= data + len) {
			*bp++ = Base64Code[c1];
			break;
		}
		c2 = *p++;
		c1 |= (c2 >> 6) & 0x03;
		*bp++ = Base64Code[c1];
		*bp++ = Base64Code[c2 & 0x3f];
	}
	*bp = '\0';
	return 0;
}

/*
 * Generates a salt for this version of crypt.
 */
int
encode_salt(char *salt, size_t saltbuflen, uint8_t *csalt, uint16_t clen,
		int log_rounds)
{
	if (saltbuflen < BCRYPT_SALTSPACE) {
		errno = EINVAL;
		return -1;
	}

	if (log_rounds < 4)
		log_rounds = 4;
	else if (log_rounds > 31)
		log_rounds = 31;

	snprintf(salt, saltbuflen, "$2b$%2.2u$", log_rounds);
	encode_base64(salt + 7, csalt, clen);

	return 0;
}

/*
 * initialization routine for the bcrypt algorithm
 */
ERL_NIF_TERM
bcrypt(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
	ErlNifBinary state;
	u_int32_t rounds;
	size_t key_len;
	u_int8_t logr, minor;
	ErlNifBinary csalt;
	char key[1024];
	char saltbuf[1024];
	const char *salt = saltbuf;
	ERL_NIF_TERM newargv[9];

	if (argc != 2)
		goto inval;

	/* Get our password and salt from argv */
	if (!enif_get_string(env, argv[0], key, sizeof(key), ERL_NIF_LATIN1))
		goto inval;
	if (!enif_get_string(env, argv[1], saltbuf, sizeof(saltbuf),
				ERL_NIF_LATIN1))
		goto inval;

	/* Check and discard "$" identifier */
	if (salt[0] != '$')
		goto inval;
	salt += 1;

	if (salt[0] != BCRYPT_VERSION)
		goto inval;

	/* Check for minor versions */
	switch ((minor = salt[1])) {
	case 'a':
		key_len = (u_int8_t)(strlen(key) + 1);
		break;
	case 'b':
		/* strlen() returns a size_t, but the function calls
		 * below result in implicit casts to a narrower integer
		 * type, so cap key_len at the actual maximum supported
		 * length here to avoid integer wraparound */
		key_len = strlen(key);
		if (key_len > 72)
			key_len = 72;
		key_len++; /* include the NUL */
		break;
	default:
		 goto inval;
	}
	if (salt[2] != '$')
		goto inval;
	/* Discard version + "$" identifier */
	salt += 3;

	/* Check and parse num rounds */
	if (!isdigit((unsigned char)salt[0]) ||
	    !isdigit((unsigned char)salt[1]) || salt[2] != '$')
		goto inval;
	logr = atoi(salt);
	if (logr < BCRYPT_MINLOGROUNDS || logr > 31)
		goto inval;
	/* Computer power doesn't increase linearly, 2^x should be fine */
	rounds = 1U << logr;

	/* Discard num rounds + "$" identifier */
	salt += 3;

	if (strlen(salt) * 3 / 4 < BCRYPT_MAXSALT)
		goto inval;

	/* We dont want the base64 salt but the raw data */
	if (!enif_alloc_binary(BCRYPT_MAXSALT, &csalt))
		goto inval;
	if (decode_base64((u_int8_t *)csalt.data, csalt.size, salt))
		goto inval_release_csalt;

	/* Setting up S-Boxes and Subkeys */
	if (!enif_alloc_binary(sizeof(blf_ctx), &state))
		goto inval_release_csalt;
	Blowfish_initstate((blf_ctx *) state.data);
	Blowfish_expandstate((blf_ctx *) state.data, (u_int8_t *) csalt.data,
			csalt.size, (u_int8_t *) key, key_len);

	/* Schedule key expansion */
	newargv[0] = enif_make_binary(env, &state);
	newargv[1] = enif_make_binary(env, &csalt);
	newargv[2] = argv[0]; /* key variable */
	newargv[3] = enif_make_ulong(env, (unsigned long)key_len);
	newargv[4] = enif_make_ulong(env, (unsigned long)rounds);
	newargv[5] = enif_make_ulong(env, (unsigned long)minor);
	newargv[6] = enif_make_ulong(env, (unsigned long)logr);
	newargv[7] = enif_make_ulong(env, 25); /* max_per_slice */
	newargv[8] = enif_make_ulong(env, 0); /* start_index */
	return enif_schedule_nif(env, "bcrypt_expandstate", 0,
			bcrypt_expandstate, 9, newargv);

inval_release_csalt:
	enif_release_binary(&csalt);
inval:
	return enif_make_badarg(env);
}

/*
 * The original bcrypt for-loop that expands the key state resembled the
 * following:
 *
 *     for (k = 0; k < rounds; k++) {
 *         Blowfish_expand0state(&state, (u_int8_t *) key, key_len);
 *         Blowfish_expand0state(&state, csalt, salt_len);
 *     }
 *
 * This simple loop has been expanded into the below routine to minimize the
 * amount of time we stay in C.  In order to meet the definition of a "quickly
 * running NIF", we should spend no more than 1 millisecond at a time in this
 * function before relinquishing control back to Erlang.
 *
 * The initial invocation of bcrypt_expandstate will attempt 25 rounds of the
 * blowfish loop and will then try to adjust that value so that subsequent
 * calls will expend as close to 1 millisecond as possible without going over.
 */
static ERL_NIF_TERM
bcrypt_expandstate(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
	u_int32_t k;
	ErlNifBinary state, csalt;
	unsigned long key_len_arg, rounds_arg, minor_arg, logr_arg;
	char key[1024];
	size_t key_len;
	u_int32_t rounds;
	int pct, total = 0;
	struct timeval start, stop;
	unsigned long max_per_slice, start_index, end;
	ERL_NIF_TERM newargv[9];

	/* Initialize our data from argv */
	if (argc != 9 || !enif_inspect_binary(env, argv[0], &state))
		return enif_make_badarg(env);
	if (!enif_inspect_binary(env, argv[1], &csalt)) {
		enif_release_binary(&state);
		return enif_make_badarg(env);
	}
	if (!enif_get_string(env, argv[2], key, sizeof(key), ERL_NIF_LATIN1) ||
	    !enif_get_ulong(env, argv[3], &key_len_arg) ||
	    !enif_get_ulong(env, argv[4], &rounds_arg) ||
	    !enif_get_ulong(env, argv[5], &minor_arg) ||
	    !enif_get_ulong(env, argv[6], &logr_arg) ||
	    !enif_get_ulong(env, argv[7], &max_per_slice) ||
	    !enif_get_ulong(env, argv[8], &start_index)) {
		enif_release_binary(&state);
		enif_release_binary(&csalt);
		return enif_make_badarg(env);
	}
	key_len = (size_t)key_len_arg;
	rounds = (u_int32_t)rounds_arg;

	/* Initialize the number of rounds this slice will try to execute */
	end = start_index + max_per_slice;
	if (end > rounds)
		end = rounds;
	k = start_index;

	/* Expand state */
	while (1) {
		/* Measure how long this slice runs */
		gettimeofday(&start, NULL);
		do {
			Blowfish_expand0state((blf_ctx *) state.data,
					(u_int8_t *) key, key_len);
			Blowfish_expand0state((blf_ctx *) state.data,
					(u_int8_t *) csalt.data, csalt.size);
		} while (++k < end);
		if (k == rounds)
			break;
		gettimeofday(&stop, NULL);

		pct = calc_percent(&total, &start, &stop);
		if (enif_consume_timeslice(env, pct)) {
			/* Schedule the next run */
			max_per_slice = adjust_max_per_slice(max_per_slice,
					total, k, start_index);

			newargv[0] = enif_make_binary(env, &state);
			newargv[1] = enif_make_binary(env, &csalt);
			newargv[2] = argv[2]; /* key variable */
			newargv[3] = argv[3]; /* key_len variable */
			newargv[4] = argv[4]; /* rounds variable */
			newargv[5] = argv[5]; /* minor variable */
			newargv[6] = argv[6]; /* logr variable */
			newargv[7] = enif_make_ulong(env, max_per_slice);
			newargv[8] = enif_make_ulong(env, k);
			return enif_schedule_nif(env, "bcrypt_expandstate", 0,
					bcrypt_expandstate, 9, newargv);
		}

		end += max_per_slice;
		if (end > rounds)
			end = rounds;
	}

	/* Schedule finalization routine */
	newargv[0] = enif_make_binary(env, &state);
	newargv[1] = enif_make_binary(env, &csalt);
	newargv[2] = argv[5]; /* minor variable */
	newargv[3] = argv[6]; /* logr variable */
	return enif_schedule_nif(env, "bcrypt_fini", 0, bcrypt_fini, 4, newargv);
}

/*
 * finalization routine for the bcrypt algorithm
 * encrypt and return the new hash
 */
static ERL_NIF_TERM
bcrypt_fini(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
	u_int32_t i, k;
	u_int16_t j;
	u_int8_t ciphertext[4 * BCRYPT_WORDS] = "OrpheanBeholderScryDoubt";
	u_int32_t cdata[BCRYPT_WORDS];
	char encrypted[BCRYPT_HASHSPACE];
	ErlNifBinary state, csalt;
	unsigned long minor_arg, logr_arg;
	u_int8_t logr, minor;

	/* Initialize our data from argv */
	if (argc != 4 || !enif_inspect_binary(env, argv[0], &state))
		return enif_make_badarg(env);
	if (!enif_inspect_binary(env, argv[1], &csalt)) {
		enif_release_binary(&state);
		return enif_make_badarg(env);
	}
	if (!enif_get_ulong(env, argv[2], &minor_arg) ||
	    !enif_get_ulong(env, argv[3], &logr_arg)) {
		enif_release_binary(&state);
		enif_release_binary(&csalt);
		return enif_make_badarg(env);
	}
	minor = (u_int8_t)minor_arg;
	logr = (u_int8_t)logr_arg;

	/* This can be precomputed later */
	j = 0;
	for (i = 0; i < BCRYPT_WORDS; i++)
		cdata[i] = Blowfish_stream2word(ciphertext, 4 * BCRYPT_WORDS, &j);

	/* Now do the encryption */
	for (k = 0; k < 64; k++)
		blf_enc((blf_ctx *) state.data, cdata, BCRYPT_WORDS / 2);

	for (i = 0; i < BCRYPT_WORDS; i++) {
		ciphertext[4 * i + 3] = cdata[i] & 0xff;
		cdata[i] = cdata[i] >> 8;
		ciphertext[4 * i + 2] = cdata[i] & 0xff;
		cdata[i] = cdata[i] >> 8;
		ciphertext[4 * i + 1] = cdata[i] & 0xff;
		cdata[i] = cdata[i] >> 8;
		ciphertext[4 * i + 0] = cdata[i] & 0xff;
	}

	snprintf(encrypted, 8, "$2%c$%2.2u$", minor, logr);
	encode_base64(encrypted + 7, (const u_int8_t *) csalt.data, BCRYPT_MAXSALT);
	encode_base64(encrypted + 7 + 22, ciphertext, 4 * BCRYPT_WORDS - 1);
	secure_bzero(state.data, state.size);
	enif_release_binary(&state);
	secure_bzero(ciphertext, sizeof(ciphertext));
	secure_bzero(csalt.data, csalt.size);
	enif_release_binary(&csalt);
	secure_bzero(cdata, sizeof(cdata));
	return enif_make_string(env, encrypted, ERL_NIF_LATIN1);
}

/*
 * A typical memset() or bzero() call can be optimized away due to "dead store
 * elimination" by sufficiently intelligent compilers.  This is a problem for
 * the above bcrypt() function which tries to zero-out several temporary
 * buffers before returning.  If these calls get optimized away, then these
 * buffers might leave sensitive information behind.  There are currently no
 * standard, portable functions to handle this issue -- thus the
 * implementation below.
 *
 * This function cannot be optimized away by dead store elimination, but it
 * will be slower than a normal memset() or bzero() call.  Given that the
 * bcrypt algorithm is designed to consume a large amount of time, the change
 * will likely be negligible.
 */
static void
secure_bzero(void *buf, size_t len)
{
	if (buf == NULL || len == 0) {
		return;
	}

	volatile unsigned char *ptr = buf;
	while (len--) {
		*ptr++ = 0;
	}
}

/*
 * Return how much of our allotted time (1 millisecond) we have used up
 * between start and stop.  Update total to reflect the percentage of time
 * that has so far been used.
 */
static int
calc_percent(int *total, struct timeval *start, struct timeval *stop)
{
	int pct;
	struct timeval diff;

	timersub(stop, start, &diff);
	pct = (int)((diff.tv_sec * 1000000 + diff.tv_usec) / 10);
	*total += pct;

	if (pct > 100)
		return 100;
	else if (pct == 0)
		return 1;
	else
		return pct;
}

/*
 * Return the optimum max_per_slice value given how much work we were able to
 * accomplish this time around.
 */
static unsigned long
adjust_max_per_slice(int max_per_slice, int total, u_int32_t k,
		unsigned long start_index)
{
	int m = total / 100;

	max_per_slice = k - start_index;
	if (m == 0)
		return max_per_slice;
	else if (m == 1)
		return max_per_slice - (max_per_slice * (total - 100) / 100);
	else
		return max_per_slice / m;
}
