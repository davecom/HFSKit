/*
 * hfsutils - tools for reading and writing Macintosh HFS volumes
 * Copyright (C) 1996-1998 Robert Leslie
 *
 * Modified by David Kopec 2/9/26
 * - Removed global variables
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
 * $Id: copyin.h,v 1.6 1998/04/11 08:26:54 rob Exp $
 */

/* copyin.h */
#ifndef HFSUTILS_COPYIN_H
#define HFSUTILS_COPYIN_H

typedef struct cpi_context {
  const char *error;
} cpi_context;

void cpi_init(cpi_context *);
const char *cpi_get_error(const cpi_context *);

typedef int (*cpifunc)(cpi_context *, const char *, hfsvol *, const char *);

int cpi_macb(cpi_context *, const char *, hfsvol *, const char *);
int cpi_binh(cpi_context *, const char *, hfsvol *, const char *);
int cpi_text(cpi_context *, const char *, hfsvol *, const char *);
int cpi_raw(cpi_context *, const char *, hfsvol *, const char *);

#endif
