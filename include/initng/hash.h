/*
 * Initng, a next generation sysvinit replacement.
 * Copyright (C) 2009 Jimmy Wennlund <jimmy.wennlund@gmail.com>
 * Copyright (C) 2009 Ismael Luceno <ismael.luceno@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef INITNG_HASH_H
#define INITNG_HASH_H

#include <stdint.h>
#include <unistd.h>

typedef uint32_t hash_t;

hash_t initng_hash(const char *key, size_t len);
hash_t initng_hash_str(const char *key);

#endif
