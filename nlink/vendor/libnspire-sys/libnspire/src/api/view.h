/*
    This file is part of libnspire.

    libnspire is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libnspire is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libnspire.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef NSP_VIEW_H
#define NSP_VIEW_H

#include <stdint.h>
#include "handle.h"

/* One frame from the resident nlink-view service (NavNet 0x40F1).
 * *out is malloc'd and owned by the caller. The bytes are the 24-byte
 * NLNKFRM1 header followed by the payload. */
int nspire_view_frame(nspire_handle_t *handle, uint8_t **out, uint32_t *out_len);

#endif
