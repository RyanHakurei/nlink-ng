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

#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "error.h"
#include "packet.h"
#include "service.h"
#include "api/view.h"

/* Must match ndless/nlink-view. Not a stock OS service. */
#define VIEW_SERVICE 0x7101
#define VIEW_HEADER 24
#define VIEW_MAX (1024u * 1024u)

static uint32_t rd_u32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 0 ok, -1 too large, -2 out of memory. */
static int append(uint8_t **buf, size_t *filled, size_t *cap,
		const uint8_t *src, size_t n, size_t limit) {
	uint8_t *grown;
	size_t ncap;

	if (n > limit || *filled > limit - n)
		return -1;
	if (*filled + n > *cap) {
		ncap = *cap ? *cap : 64;
		while (ncap < *filled + n) {
			if (ncap > limit / 2) {
				ncap = limit;
				break;
			}
			ncap *= 2;
		}
		if (ncap > limit)
			ncap = limit;
		grown = realloc(*buf, ncap);
		if (!grown)
			return -2;
		*buf = grown;
		*cap = ncap;
	}
	memcpy(*buf + *filled, src, n);
	*filled += n;
	return 0;
}

int nspire_view_frame(nspire_handle_t *handle, uint8_t **out, uint32_t *out_len) {
	int ret, disc, app;
	uint32_t maxds, length;
	uint8_t *packet = NULL;
	uint8_t *buf = NULL;
	size_t filled = 0, cap = 0, need = 0, got;
	unsigned reads = 0;

	if (!handle || !out || !out_len)
		return -NSPIRE_ERR_INVALID;
	*out = NULL;
	*out_len = 0;

	if ((ret = service_connect(handle, VIEW_SERVICE)))
		return ret;

	maxds = packet_max_datasize(handle);
	if (maxds < 16) {
		ret = -NSPIRE_ERR_INVALID;
		goto end;
	}
	packet = malloc(maxds);
	if (!packet) {
		ret = -NSPIRE_ERR_NOMEM;
		goto end;
	}

	{
		struct packet request = packet_new(handle);
		request.data[0] = 0x01;
		request.data_size = 1;
		/* Do not wait for this send to finish. The calculator answers
		 * before that wait would return, and the keypad stays frozen. */
		if ((ret = packet_send_nowait(handle, request)))
			goto end;
	}

	while (need == 0 || filled < need) {
		struct packet incoming;
		if (++reads > 8192) {
			ret = -NSPIRE_ERR_INVALID;
			goto end;
		}
		if ((ret = packet_recv(handle, &incoming)))
			goto end;
		/* ACKs are not frame bytes. A user service waits for a classic
		 * ACK before it sends the next piece, including on CX II. */
		if (incoming.src_sid == 0xFF || incoming.src_sid == 0xFE)
			continue;
		if (incoming.src_sid == 0xD3) {
			ret = -NSPIRE_ERR_INVALPKT;
			goto end;
		}
		if (!handle->is_cx2 && incoming.dst_sid != handle->host_sid)
			continue;
		got = packet_datasize(&incoming);
		if (got == 0 || got > maxds) {
			ret = -NSPIRE_ERR_INVALID;
			goto end;
		}
		memcpy(packet, packet_dataptr(&incoming), got);
		if ((ret = packet_ack(handle, incoming)))
			goto end;
		app = append(&buf, &filled, &cap, packet, got, need ? need : VIEW_MAX);
		if (app == -2) {
			ret = -NSPIRE_ERR_NOMEM;
			goto end;
		}
		if (app != 0) {
			ret = -NSPIRE_ERR_INVALID;
			goto end;
		}
		if (need == 0 && filled >= VIEW_HEADER) {
			if (memcmp(buf, "NLNKFRM1", 8) != 0) {
				ret = -NSPIRE_ERR_INVALID;
				goto end;
			}
			length = rd_u32(buf + 20);
			if (length > VIEW_MAX - VIEW_HEADER) {
				ret = -NSPIRE_ERR_INVALID;
				goto end;
			}
			need = VIEW_HEADER + (size_t)length;
			if (filled > need) {
				ret = -NSPIRE_ERR_INVALID;
				goto end;
			}
		}
	}

	*out = buf;
	*out_len = (uint32_t)filled;
	buf = NULL;
	ret = NSPIRE_ERR_SUCCESS;
end:
	disc = service_disconnect(handle);
	free(packet);
	free(buf);
	if (ret == NSPIRE_ERR_SUCCESS && disc)
		return disc;
	return ret;
}
