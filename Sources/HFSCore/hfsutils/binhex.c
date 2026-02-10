/*
 * hfsutils - tools for reading and writing Macintosh HFS volumes
 * Copyright (C) 1996-1998 Robert Leslie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: binhex.c,v 1.11 1998/09/18 22:56:22 rob Exp $
 */

# ifdef HAVE_CONFIG_H
#  include "config.h"
# endif

# ifdef HAVE_UNISTD_H
#  include <unistd.h>
# else
int dup(int);
# endif

# include <stdio.h>
# include <string.h>
# include <errno.h>

# include "binhex.h"
# include "crc.h"

extern int errno;

# define ERROR(ctx, code, str)	((ctx)->error = (str), errno = (code))

static const
unsigned char zero[2] = { 0, 0 };

static const
char hqxheader[] = "(This file must be converted with BinHex 4.0)\n";

static const
char enmap[] = "!\"#$%&'()*+,-012345689@ABCDEFGHI"
	       "JKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

static const
signed char demap[256] = {
   0,  0,  0,  0,  0,  0,  0,  0,
   0, -1, -1,  0,  0, -1,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0,
  -1,  1,  2,  3,  4,  5,  6,  7,
   8,  9, 10, 11, 12, 13,  0,  0,
  14, 15, 16, 17, 18, 19, 20,  0,
  21, 22,  0,  0,  0,  0,  0,  0,
  23, 24, 25, 26, 27, 28, 29, 30,
  31, 32, 33, 34, 35, 36, 37,  0,
  38, 39, 40, 41, 42, 43, 44,  0,
  45, 46, 47, 48,  0,  0,  0,  0,
  49, 50, 51, 52, 53, 54, 55,  0,
  56, 57, 58, 59, 60, 61,  0,  0,
  62, 63, 64,  0,  0,  0,  0,  0,
   0,  0,  0,  0,  0,  0,  0,  0
};

# define HEADERMATCH	40
# define MAXLINELEN	64
# define ISRETURN(c)	(demap[(unsigned char) (c)] == -1)

/* BinHex Encoding ========================================================= */

void bh_init(bh_context *ctx)
{
  memset(ctx, 0, sizeof(*ctx));
  ctx->error = "no error";
}

const char *bh_get_error(const bh_context *ctx)
{
  return ctx->error;
}

/*
 * NAME:	bh->start()
 * DESCRIPTION:	begin BinHex encoding
 */
int bh_start(bh_context *ctx, int fd)
{
  int dupfd;
  FILE *file;

  dupfd = dup(fd);
  if (dupfd == -1)
    {
      ERROR(ctx, errno, "error duplicating output stream");
      return -1;
    }

  file = fdopen(dupfd, "ab");
  if (file == 0)
    {
      ERROR(ctx, errno, "error creating output buffer");

      close(dupfd);
      return -1;
    }

  ctx->file = file;
  ctx->line[0] = ':';
  ctx->lptr = 1;

  ctx->state86 = 0;
  ctx->runlen  = 0;

  ctx->crc = 0x0000;

  if (fputs(hqxheader, file) == EOF)
    {
      ERROR(ctx, EIO, "error writing hqx header");

      fclose(file);
      return -1;
    }

  return 0;
}

/*
 * NAME:	flushline()
 * DESCRIPTION:	flush a line to the output file
 */
static
int flushline(bh_context *ctx)
{
  FILE *file = (FILE *)ctx->file;

  ctx->line[ctx->lptr++] = '\n';
  ctx->line[ctx->lptr++] = 0;

  if (fputs(ctx->line, file) == EOF)
    {
      ERROR(ctx, EIO, "error writing output data");
      return -1;
    }

  ctx->lptr = 0;

  return 0;
}

/*
 * NAME:	addchars()
 * DESCRIPTION:	insert bytes of data to the output stream
 */
static
int addchars(bh_context *ctx, const unsigned char *data, register int len)
{
  register unsigned char c;

  while (len--)
    {
      c = *data++;

      if (ctx->lptr == MAXLINELEN &&
	  flushline(ctx) == -1)
	return -1;

      switch (ctx->state86 & 0xff00)
	{
	case 0x0000:
	  ctx->line[ctx->lptr++] = enmap[c >> 2];
	  ctx->state86 = 0x0100 | (c & 0x03);
	  break;

	case 0x0100:
	  ctx->line[ctx->lptr++] = enmap[((ctx->state86 & 0x03) << 4) | (c >> 4)];
	  ctx->state86 = 0x0200 | (c & 0x0f);
	  break;

	case 0x0200:
	  ctx->line[ctx->lptr++] = enmap[((ctx->state86 & 0x0f) << 2) | (c >> 6)];

	  if (ctx->lptr == MAXLINELEN &&
	      flushline(ctx) == -1)
	    return -1;

	  ctx->line[ctx->lptr++] = enmap[c & 0x3f];
	  ctx->state86 = 0;
	  break;
	}
    }

  return 0;
}

/*
 * NAME:	rleflush()
 * DESCRIPTION:	run-length encode data
 */
static
int rleflush(bh_context *ctx)
{
  unsigned char rle[] = { 0x90, 0x00, 0x90, 0x00 };

  if ((ctx->lastch != 0x90 && ctx->runlen < 4) ||
      (ctx->lastch == 0x90 && ctx->runlen < 3))
    {
      /* self representation */

      if (ctx->lastch == 0x90)
	{
	  while (ctx->runlen--)
	    if (addchars(ctx, rle, 2) == -1)
	      return -1;
	}
      else
	{
	  while (ctx->runlen--)
	    if (addchars(ctx, &ctx->lastch, 1) == -1)
	      return -1;
	}
    }
  else
    {
      /* run-length encoded */

      if (ctx->lastch == 0x90)
	{
	  rle[3] = ctx->runlen;

	  if (addchars(ctx, rle, 4) == -1)
	    return -1;
	}
      else
	{
	  rle[1] = ctx->lastch;
	  rle[3] = ctx->runlen;

	  if (addchars(ctx, &rle[1], 3) == -1)
	    return -1;
	}
    }

  ctx->runlen = 0;

  return 0;
}

/*
 * NAME:	bh->insert()
 * DESCRIPTION:	encode bytes of data, buffering lines and flushing
 */
int bh_insert(bh_context *ctx, const void *buf, register int len)
{
  register const unsigned char *data = buf;

  ctx->crc = crc_binh(data, len, ctx->crc);

  for ( ; len--; ++data)
    {
      if (ctx->runlen)
	{
	  if (ctx->runlen == 0xff || ctx->lastch != *data)
	    {
	      if (rleflush(ctx) == -1)
		return -1;
	    }

	  if (ctx->lastch == *data)
	    {
	      ++ctx->runlen;
	      continue;
	    }
	}

      ctx->lastch = *data;
      ctx->runlen = 1;
    }

  return 0;
}

/*
 * NAME:	bh->insertcrc()
 * DESCRIPTION:	insert a two-byte CRC checksum
 */
int bh_insertcrc(bh_context *ctx)
{
  unsigned char word[2];

  ctx->crc = crc_binh(zero, 2, ctx->crc);

  word[0] = (ctx->crc & 0xff00) >> 8;
  word[1] = (ctx->crc & 0x00ff) >> 0;

  if (bh_insert(ctx, word, 2) == -1)
    return -1;

  ctx->crc = 0x0000;

  return 0;
}

/*
 * NAME:	bh->end()
 * DESCRIPTION:	finish BinHex encoding
 */
int bh_end(bh_context *ctx)
{
  int result = 0;
  FILE *file = (FILE *)ctx->file;

  if (ctx->runlen &&
      rleflush(ctx) == -1)
    result = -1;

  if (ctx->state86 && result == 0 &&
      addchars(ctx, zero, 1) == -1)
    result = -1;

  ctx->line[ctx->lptr++] = ':';

  if (result == 0 &&
      flushline(ctx) == -1)
    result = -1;

  if (fclose(file) == EOF && result == 0)
    {
      ERROR(ctx, errno, "error flushing output data");
      result = -1;
    }

  return result;
}

/* BinHex Decoding ========================================================= */

/*
 * NAME:	bh->open()
 * DESCRIPTION:	begin BinHex decoding
 */
int bh_open(bh_context *ctx, int fd)
{
  int dupfd, c;
  const char *ptr;
  FILE *file;

  dupfd = dup(fd);
  if (dupfd == -1)
    {
      ERROR(ctx, errno, "error duplicating input stream");
      return -1;
    }

  file = fdopen(dupfd, "rb");
  if (file == 0)
    {
      ERROR(ctx, errno, "error creating input buffer");

      close(dupfd);
      return -1;
    }

  ctx->file = file;
  ctx->state86 = 0;
  ctx->runlen  = 0;

  ctx->crc = 0x0000;

  /* find hqx header */

  ptr = hqxheader;
  while (ptr == 0 || ptr - hqxheader < HEADERMATCH)
    {
      c = getc(file);
      if (c == EOF)
	{
	  ERROR(ctx, EINVAL, "hqx file header not found");

	  fclose(file);
	  return -1;
	}

      if (c == '\n' || c == '\r')
	{
	  ptr = hqxheader;
	  continue;
	}

      if (ptr && c != *ptr++)
	ptr = 0;
    }

  /* skip to CR/LF */

  do
    {
      c = getc(file);
      if (c == EOF)
	{
	  ERROR(ctx, EINVAL, "corrupt hqx file");

	  fclose(file);
	  return -1;
	}
    }
  while (c != '\n' && c != '\r');

  /* skip whitespace */

  do
    {
      c = getc(file);
      if (c == EOF)
	{
	  ERROR(ctx, EINVAL, "corrupt hqx file");

	  fclose(file);
	  return -1;
	}
    }
  while (ISRETURN(c));

  if (c != ':')
    {
      ERROR(ctx, EINVAL, "corrupt hqx file");

      fclose(file);
      return -1;
    }

  return 0;
}

/*
 * NAME:	hqxchar()
 * DESCRIPTION:	return the next hqx character from the input stream
 */
static
int hqxchar(bh_context *ctx)
{
  FILE *file = (FILE *)ctx->file;
  int c;

  do
    c = getc(file);
  while (c != EOF && ISRETURN(c));

  if (c == EOF)
    {
      if (feof(file))
	ERROR(ctx, EINVAL, "unexpected end of file");
      else
	ERROR(ctx, EIO, "error reading input file");

      return -1;
    }

  c = demap[(unsigned char) c];
  if (c == 0)
    {
      ERROR(ctx, EINVAL, "illegal character in hqx file");
      return -1;
    }

  return c - 1;
}

/*
 * NAME:	nextchar()
 * DESCRIPTION:	decode one character from the hqx stream
 */
static
int nextchar(bh_context *ctx)
{
  int c, c2, ch;

  c = hqxchar(ctx);
  if (c == -1)
    return -1;

  switch (ctx->state86 & 0xff00)
    {
    case 0x0000:
      c2 = hqxchar(ctx);
      if (c2 == -1)
	return -1;

      ch = (c << 2) | (c2 >> 4);
      ctx->state86 = 0x0100 | (c2 & 0x0f);
      break;

    case 0x0100:
      ch = ((ctx->state86 & 0x0f) << 4) | (c >> 2);
      ctx->state86 = 0x0200 | (c & 0x03);
      break;

    case 0x0200:
      ch = ((ctx->state86 & 0x03) << 6) | c;
      ctx->state86 = 0;
      break;
    }

  return ch;
}

/*
 * NAME:	bh->read()
 * DESCRIPTION:	decode and return bytes from the hqx stream
 */
int bh_read(bh_context *ctx, void *buf, register int len)
{
  register unsigned char *data = buf;
  const unsigned char *ptr = data;
  int c, rl, count = len;

  while (len--)
    {
      if (ctx->runlen)
	{
	  *data++ = ctx->lastch;
	  --ctx->runlen;
	  continue;
	}

      c = nextchar(ctx);
      if (c == -1)
	return -1;

      if (c == 0x90)
	{
	  rl = nextchar(ctx);
	  if (rl == -1)
	    return -1;

	  if (rl > 0)
	    {
	      ctx->runlen = rl - 1;
	      ++len;
	      continue;
	    }
	}

      *data++ = ctx->lastch = c;
    }

  ctx->crc = crc_binh(ptr, count, ctx->crc);

  return count;
}

/*
 * NAME:	bh->readcrc()
 * DESCRIPTION:	read and compare CRC bytes
 */
int bh_readcrc(bh_context *ctx)
{
  unsigned short check;
  unsigned char word[2];

  check = crc_binh(zero, 2, ctx->crc);

  if (bh_read(ctx, word, 2) < 2)
    return -1;

  ctx->crc = (word[0] << 8) |
             (word[1] << 0);

  if (ctx->crc != check)
    {
      ERROR(ctx, EINVAL, "CRC checksum error");
      return -1;
    }

  ctx->crc = 0x0000;

  return 0;
}

/*
 * NAME:	bh->close()
 * DESCRIPTION:	finish BinHex decoding
 */
int bh_close(bh_context *ctx)
{
  FILE *file = (FILE *)ctx->file;
  int c, result = 0;

  /* skip whitespace */

  do
    c = getc(file);
  while (c != EOF && ISRETURN(c));

  /* skip optional exclamation */

  if (c == '!')
    {
      do
	c = getc(file);
      while (c != EOF && ISRETURN(c));
    }

  /* verify trailing colon */

  if (c != ':')
    {
      ERROR(ctx, EINVAL, "corrupt end of hqx file");
      result = -1;
    }

  if (fclose(file) == EOF && result == 0)
    {
      ERROR(ctx, errno, "error closing input file");
      result = -1;
    }

  return result;
}
