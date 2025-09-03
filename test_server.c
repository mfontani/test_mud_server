/******************************************************************************
 * vim: set ts=2 sw=2 sts=2 et:
 * test_server.c - MCCP4 Test Server
 *
 * A simple telnet server that demonstrates MCCP4/MCCP2 implementation.
 *
 * Compile:
 *      gcc -o test_server test_server.c -lzstd -lz
 * Usage:
 *      ./test_server [port]
 *
 * Copyright (c) 2025 Jeffrey Johnston (Demon) of Dark Wizardry - https://www.github.com/Coffee-Nerd
 * Copyright (c) 2025 Marco Fontani (mfontani) - mf@marcofontani.it
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 ******************************************************************************/

#include <arpa/inet.h>
#include <arpa/telnet.h>
#include <asm-generic/errno-base.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>

#define DEFAULT_PORT 4000
#define BUFFER_SIZE 4096

#define TELOPT_COMPRESS2 86
#define TELOPT_COMPRESS4 88
#define MCCP4_ACCEPT_ENCODING 1
#define MCCP4_BEGIN_ENCODING 2

enum telnet_state
{
  TELNET_STATE_TEXT,       /* Default: we're receiving text, and appending to input buffer */
  TELNET_STATE_IAC,        /* While in TELNET_STATE_TEXT, we just got an IAC. */
  TELNET_STATE_IAC_SB,     /* We got an IAC SB, are awaiting ending IAC SE */
  TELNET_STATE_IAC_SB_IAC, /* We got IAC, SB, optional data, and IAC again. Awaiting SE. */
  TELNET_STATE_IAC_DONT,   /* We got IAC DONT, awaiting option byte */
  TELNET_STATE_IAC_DO,     /* We got IAC DO, awaiting option byte */
  TELNET_STATE_IAC_WONT,   /* We got IAC WONT, awaiting option byte */
  TELNET_STATE_IAC_WILL,   /* We got IAC WILL, awaiting option byte */
};

/* Simple descriptor structure */
typedef struct
{
  struct
  {
    int socket;                                /* The FD */
    char inet_name[64 + INET6_ADDRSTRLEN + 1]; /* pid[addr:port] */
  } connection;
  struct
  {
    enum telnet_state state;   /* State of the connection */
    unsigned char sb_data[64]; /* IAC SB ... IAC SE data */
    size_t sb_pos;             /* Index of next element to be added */
  } telnet;
  struct
  {
    unsigned char ibuf[BUFFER_SIZE]; /* TEXT input from socket */
    size_t ipos;                     /* Index of next element to be added */
  } input;
  struct
  {
    char obuf[8192];  /* Output buffer, sent at end of loop */
    size_t opos;      /* Where to append at end of buffer */
    size_t total_out; /* Total bytes sent */
  } output;
  struct
  {
    bool can_mccp4;             /* Client said they could do MCCP4 */
    ZSTD_CStream *zstd_stream;  /* per-descriptor zstd "state" structure */
    unsigned char *zstd_outbuf; /* compressed data is put here */
    size_t zstd_outcap;         /* the size of the allocated zstd_outbuf */
    size_t zstd_outpos;         /* up to where data has been put in zstd_outbuf */
    size_t mccp4_bytes_in;      /* Total bytes sent to compressor */
    size_t mccp4_bytes_out;     /* Total compressed bytes sent to socket */
  } mccp4;
  struct
  {
    /* MCCP2: */
    bool can_mccp2;         /* Client said they could do MCCP2 */
    z_stream *zstate;       /* per-descriptor "state" of MCCP2 compression */
    size_t mccp2_bytes_in;  /* Total bytes sent to compressor */
    size_t mccp2_bytes_out; /* Total compressed bytes sent to socket */
  } mccp2;
  size_t uncompressed_out; /* Total bytes out sent sans compression */
} descriptor_t;

/* Compression "ratio"s:
 * * +01.00% for when  9900 bytes are output for 10000 bytes of input.
 * * +00.00% for when 10000 bytes are output for 10000 bytes of input.
 * * -10.00% for when 11000 bytes are output for 10000 bytes of input.
 */
#define DRATIO(out, in) (double)((in != 0) ? (100.0 * (1.0 - ((double)(out) / (double)(in)))) : 0.0)

/* A greeting shown at connection time. */
const char *initial_greeting = "Welcome to the MCCP2/MCCP4 test server!";

/* write() all data in the output buffer of the descriptor. Returns false on error. */
bool flush_desc(descriptor_t *d) __attribute__((nonnull(1), warn_unused_result));
bool flush_desc(descriptor_t *d)
{
  if (d->output.opos > 0)
  {
    printf("DEBUG: %s: Flushing %9zu bytes...\n", d->connection.inet_name, d->output.opos);
    ssize_t ret = d->output.opos;
    char *from_send = d->output.obuf;
    while (ret > 0)
    {
      ssize_t to_send = ret;
      ret = write(d->connection.socket, from_send, to_send);
      if (ret == to_send)
      {
        // All sent!
        d->output.opos = 0;
        return true;
      }
      if (ret < 0)
      {
        printf("DEBUG: %s: write error: %s\n", d->connection.inet_name, strerror(errno));
        return false;
      }
      // Still more to send. Next loop!
      printf(
          "DEBUG: %s: didn't flush all in one write(). Another for %ld/%ld...\n", d->connection.inet_name, ret, to_send
      );
    }
    // Shouldn't happen ...
    abort();
  }
  else
  {
    printf("DEBUG: %s: Nothing to flush!\n", d->connection.inet_name);
    return true;
  }
}

/*
 * Place data[len], compressing as required, in the descriptor's output buffer.
 * Auto-flushes the output buffer if/once about to be full.
 * Returns false on failure.
 */
bool send_to_desc(descriptor_t *d, const char *data, int len) __attribute__((nonnull(1, 2), warn_unused_result));
bool send_to_desc(descriptor_t *d, const char *data, int len)
{
  if (len <= 0)
    len = strlen(data);

  printf("DEBUG: %s: send_to_desc len %d, buffer pos %zu...\n", d->connection.inet_name, len, d->output.opos);

  if (d->mccp4.zstd_stream)
  {
    /* Track bytes going into compressor */
    d->mccp4.mccp4_bytes_in += len;

    /* Compress data */
    ZSTD_inBuffer in = {data, len, 0};
    size_t input_len = len;
    size_t output_len = 0;

    while (in.pos < in.size)
    {
      ZSTD_outBuffer out = {
          d->mccp4.zstd_outbuf + d->mccp4.zstd_outpos, d->mccp4.zstd_outcap - d->mccp4.zstd_outpos, 0
      };

      size_t ret = ZSTD_compressStream2(d->mccp4.zstd_stream, &out, &in, ZSTD_e_flush);
      if (ZSTD_isError(ret))
      {
        printf("Server: %s: Compression error: %s\n", d->connection.inet_name, ZSTD_getErrorName(ret));
        return false;
      }
      output_len += out.pos;

      d->mccp4.zstd_outpos += out.pos;

      /* Flush if buffer is getting full */
      if (d->mccp4.zstd_outpos >= d->mccp4.zstd_outcap / 2)
      {
        printf(
            "DEBUG: %s: outpos %zu >= outcap/2 %zu, flushing!\n", d->connection.inet_name, d->mccp4.zstd_outpos,
            d->mccp4.zstd_outcap / 2
        );
        if (!flush_desc(d))
          return false;
        if (d->output.opos + d->mccp4.zstd_outpos >= sizeof(d->output.obuf))
        {
          // Can't do much here...
          printf("BUG: %s: can't append %zu!\n", d->connection.inet_name, d->mccp4.zstd_outpos);
          return false;
        }
      }
      // Append to output buffer
      memcpy(d->output.obuf + d->output.opos, d->mccp4.zstd_outbuf, d->mccp4.zstd_outpos);
      d->output.opos += d->mccp4.zstd_outpos;
      /* Track actual compressed bytes sent */
      d->mccp4.mccp4_bytes_out += d->mccp4.zstd_outpos;
      d->mccp4.zstd_outpos = 0;

      if (in.pos == in.size && ret == 0)
        break;
    }
    printf(
        "DEBUG: %s: MCCP4: Appended %5zu for %5zu len, %+8.2f%% compression ratio,"
        "%9zu for %9zu len so far, %+8.2f%% ratio.\n",
        d->connection.inet_name, output_len, input_len, DRATIO(output_len, input_len), d->mccp4.mccp4_bytes_out,
        d->mccp4.mccp4_bytes_in, DRATIO(d->mccp4.mccp4_bytes_out, d->mccp4.mccp4_bytes_in)
    );
  }
  else if (d->mccp2.zstate)
  {
    /* MCCP2 compression */
    d->mccp2.mccp2_bytes_in += len;

    d->mccp2.zstate->next_in = (Bytef *)data;
    d->mccp2.zstate->avail_in = len;

    int input_len = len;
    int output_len = 0;
    while (d->mccp2.zstate->avail_in > 0)
    {
      unsigned char outbuf[4096];
      d->mccp2.zstate->next_out = outbuf;
      d->mccp2.zstate->avail_out = sizeof(outbuf);

      int ret = deflate(d->mccp2.zstate, Z_SYNC_FLUSH);
      if (ret != Z_OK)
      {
        printf("Server: %s: MCCP2 deflate error: %d\n", d->connection.inet_name, ret);
        return false;
      }

      size_t have = sizeof(outbuf) - d->mccp2.zstate->avail_out;
      if (have > 0)
      {
        if (d->output.opos + have >= sizeof(d->output.obuf))
        {
          // Flush ...
          if (!flush_desc(d))
            return false;
        }
        memcpy(d->output.obuf + d->output.opos, outbuf, have);
        d->output.opos += have;
        d->mccp2.mccp2_bytes_out += have;
        output_len += have;
      }
    }
    printf(
        "DEBUG: %s: MCCP2: Appended %5d for %5d len, %+8.2f%% compression ratio, %9zu for %9zu len so far, %+8.2f%% "
        "ratio.\n",
        d->connection.inet_name, output_len, input_len, DRATIO(output_len, input_len), d->mccp2.mccp2_bytes_out,
        d->mccp2.mccp2_bytes_in, DRATIO(d->mccp2.mccp2_bytes_out, d->mccp2.mccp2_bytes_in)
    );
  }
  else
  {
    d->uncompressed_out += len;
    if (d->output.opos + len < sizeof(d->output.obuf))
    {
      memcpy(d->output.obuf + d->output.opos, data, len);
      d->output.opos += len;
      return true;
    }
    // Overflow. Send existing data, then append.
    printf("DEBUG: %s: Overflow! Flushing, then appending.\n", d->connection.inet_name);
    if (!flush_desc(d))
      return false;
    if (!send_to_desc(d, data, len))
      return false;
  }
  return true;
}

bool start_mccp4_compression(descriptor_t *d, int level) __attribute__((nonnull(1), warn_unused_result));
bool start_mccp4_compression(descriptor_t *d, int level)
{
  if (d->mccp4.zstd_stream)
    return true;
  if (d->mccp2.zstate)
    return true;

  printf(
      "DEBUG: %s: start_mccp4_compression for descriptor %d level=%d\n", d->connection.inet_name, d->connection.socket,
      level
  );

  /* Send BEGIN_ENCODING */
  unsigned char begin[] = {IAC, SB, TELOPT_COMPRESS4, MCCP4_BEGIN_ENCODING, 'z', 's', 't', 'd', IAC, SE};
  if (!send_to_desc(d, (char *)begin, sizeof(begin)))
    return false;
  printf("DEBUG: %s: Sending BEGIN_ENCODING 'zstd' to descriptor %d\n", d->connection.inet_name, d->connection.socket);

  /* Create compression stream */
  d->mccp4.zstd_stream = ZSTD_createCStream();
  if (!d->mccp4.zstd_stream)
    return false;

  size_t ret = ZSTD_initCStream(d->mccp4.zstd_stream, level);
  if (ZSTD_isError(ret))
  {
    printf("BUG: %s: ZSTD_initCStream error: %s\n", d->connection.inet_name, ZSTD_getErrorName(ret));
    d->mccp4.zstd_stream = NULL;
    return false;
  }
  /* Allocate output buffer */
  d->mccp4.zstd_outcap = ZSTD_CStreamOutSize();
  d->mccp4.zstd_outbuf = malloc(d->mccp4.zstd_outcap);
  if (!d->mccp4.zstd_outbuf)
  {
    perror("BUG: malloc");
    ZSTD_freeCStream(d->mccp4.zstd_stream);
    d->mccp4.zstd_stream = NULL;
    return false;
  }
  printf("DEBUG: %s: allocated %zu bytes for zstd_outbuf\n", d->connection.inet_name, d->mccp4.zstd_outcap);

  d->mccp4.zstd_outpos = 0;

  return true;
}

/* Ends MCCP4 compression, if enabled. Returns false on failure. */
bool end_mccp4_compression(descriptor_t *d) __attribute__((nonnull(1), warn_unused_result));
bool end_mccp4_compression(descriptor_t *d)
{
  if (!d->mccp4.zstd_stream)
    return true;
  printf("DEBUG: end_mccp4_compression starting - descriptor %d %s\n", d->connection.socket, d->connection.inet_name);

  ZSTD_inBuffer in = {NULL, 0, 0};

  /* LOOP until frame is completely ended (like real implementation) */
  for (;;)
  {
    ZSTD_outBuffer out = {d->mccp4.zstd_outbuf + d->mccp4.zstd_outpos, d->mccp4.zstd_outcap - d->mccp4.zstd_outpos, 0};

    size_t zr = ZSTD_compressStream2(d->mccp4.zstd_stream, &out, &in, ZSTD_e_end);

    /* Check for errors */
    if (ZSTD_isError(zr))
    {
      printf("DEBUG: %s: ZSTD error during frame end for: %s\n", d->connection.inet_name, ZSTD_getErrorName(zr));
      return false;
    }

    d->mccp4.zstd_outpos += out.pos;
    printf("DEBUG: %s: ZSTD frame end - produced %zu bytes, zr=%zu\n", d->connection.inet_name, out.pos, zr);

    /* Frame completely ended? */
    if (zr == 0)
    {
      printf("DEBUG: %s: ZSTD frame successfully ended\n", d->connection.inet_name);
      break;
    }
    printf("DEBUG: %s: ZSTD frame still %zu to go through...", d->connection.inet_name, zr);
  }

  if (d->mccp4.zstd_outpos > 0)
  {
    if (d->output.opos + d->mccp4.zstd_outpos >= sizeof(d->output.obuf))
    {
      if (!flush_desc(d))
        return false;
    }
    memcpy(d->output.obuf + d->output.opos, d->mccp4.zstd_outbuf, d->mccp4.zstd_outpos);
    d->output.opos += d->mccp4.zstd_outpos;
  }

  // Send all data!
  printf("DEBUG: %s: Flushing pending data ...\n", d->connection.inet_name);
  if (!flush_desc(d))
    return false;

  /* Clean up resources */
  ZSTD_freeCStream(d->mccp4.zstd_stream);
  d->mccp4.zstd_stream = NULL;
  free(d->mccp4.zstd_outbuf);
  d->mccp4.zstd_outbuf = NULL;
  printf("DEBUG: MCCP4 cleanup completed for %s\n", d->connection.inet_name);
  return true;
}

bool start_mccp2_compression(descriptor_t *d, int level) __attribute__((nonnull(1), warn_unused_result));
bool start_mccp2_compression(descriptor_t *d, int level)
{
  if (d->mccp4.zstd_stream)
    return true;
  if (d->mccp2.zstate)
    return true;

  printf(
      "DEBUG: %s: MCCP2 start called for descriptor %d, level=%d\n", d->connection.inet_name, d->connection.socket,
      level
  );

  z_stream *zstate = (z_stream *)calloc(1, sizeof(z_stream));
  if (!zstate)
  {
    printf("BUG: %s: calloc for z_stream=%zu!\n", d->connection.inet_name, sizeof(z_stream));
    return false;
  }

  zstate->data_type = Z_TEXT;
  if (deflateInit(zstate, level) != Z_OK)
  {
    printf("BUG: %s: Cannot initialize z_stream level %d: %s", d->connection.inet_name, level, zstate->msg);
    free(zstate->msg);
    free(zstate);
    return false;
  }

  /* Send BEGIN_ENCODING */
  unsigned char begin[] = {IAC, SB, TELOPT_COMPRESS2, IAC, SE};
  if (!send_to_desc(d, (char *)begin, sizeof(begin)))
  {
    free(zstate);
    return false;
  }
  printf("DEBUG: %s: Sent begin MCCP2 to descriptor %d\n", d->connection.inet_name, d->connection.socket);
  // Need to flush!
  if (!flush_desc(d))
    return false;
  d->mccp2.zstate = zstate;
  return true;
}

/* Ends MCCP2 compression, if enabled. Returns false on failure. */
bool end_mccp2_compression(descriptor_t *d) __attribute__((nonnull(1), warn_unused_result));
bool end_mccp2_compression(descriptor_t *d)
{
  if (!d->mccp2.zstate)
    return true;
  printf("DEBUG: end_mccp2_compression starting - descriptor %d %s\n", d->connection.socket, d->connection.inet_name);

  char buffer[256];
  d->mccp2.zstate->next_in = NULL;
  d->mccp2.zstate->avail_in = 0;
  d->mccp2.zstate->next_out = (Bytef *)buffer;
  d->mccp2.zstate->avail_out = sizeof(buffer);
  int ret = deflate(d->mccp2.zstate, Z_FINISH);
  if (ret == Z_STREAM_END && d->mccp2.zstate->avail_out)
  {
    int to_write = sizeof(buffer) - d->mccp2.zstate->avail_out;
    if (d->output.opos + to_write >= sizeof(d->output.obuf))
    {
      // Flush ...
      if (!flush_desc(d))
        return false;
    }
    memcpy(d->output.obuf + d->output.opos, buffer, to_write);
    d->output.opos += to_write;
  }
  d->mccp2.zstate->next_out = NULL;
  deflateEnd(d->mccp2.zstate);
  free(d->mccp2.zstate);
  d->mccp2.zstate = NULL;
  printf("DEBUG: MCCP2 cleanup completed for %s\n", d->connection.inet_name);
  return true;
}

const char *help_page = "Valid commands:\r\n"
                        "- quit        Close the connection.\r\n"
                        "- mccp4       Start MCCP4 compression, if not already on/MCCP2 not on.\r\n"
                        "- mccp2       Start MCCP2 compression, if not already on/MCCP4 not on.\r\n"
                        "- off         Turn off any type of compression you have on.\r\n"
                        "- test        The server will send you some test text.\r\n"
                        "- stats       Shows connection/compression statistics.\r\n"
                        "- help        Shows this help page.\r\n"
                        "Anything else will get echoed back to you.\r\n"
                        "";

bool process_command(descriptor_t *d, char *line, int line_pos) __attribute__((nonnull(1, 2), warn_unused_result));
bool process_command(descriptor_t *d, char *line, int line_pos)
{
  (void)line_pos; /* currently unused parameter */
  if (strcmp(line, "help") == 0)
  {
    if (!send_to_desc(d, help_page, 0))
      return false;
    return true;
  }
  if (strcmp(line, "quit") == 0)
  {
    if (!send_to_desc(d, "Goodbye!\r\n", 0))
      return false;
    if (!flush_desc(d))
      return false;
    return false;
  }
  if (strcmp(line, "mccp4") == 0)
  {
    if (d->mccp4.zstd_stream)
    {
      if (!send_to_desc(d, "Already compressing using MCCP4.\r\n", 0))
        return false;
    }
    else if (d->mccp2.zstate)
    {
      if (!send_to_desc(d, "Already compressing using MCCP2.\r\n", 0))
        return false;
    }
    else
    {
      if (!start_mccp4_compression(d, 9))
        return false;
      if (!send_to_desc(d, "MCCP4 Compression level 9 enabled!\r\n", 0))
        return false;
    }
    return true;
  }
  if (strcmp(line, "mccp2") == 0)
  {
    if (d->mccp4.zstd_stream)
    {
      if (!send_to_desc(d, "Already compressing using MCCP4.\r\n", 0))
        return false;
    }
    else if (d->mccp2.zstate)
    {
      if (!send_to_desc(d, "Already compressing using MCCP2.\r\n", 0))
        return false;
    }
    else
    {
      if (!start_mccp2_compression(d, -1)) // = Z_DEFAULT_COMPRESSION
        return false;
      if (!send_to_desc(d, "MCCP2 Compression level -1 enabled!\r\n", 0))
        return false;
    }
    return true;
  }
  if (strcmp(line, "test") == 0)
  {
    /* Send test data */
    if (!send_to_desc(d, "Sending test data...\r\n", 0))
      return false;
    char msg[256];
    for (int j = 0; j < 10; j++)
    {
      snprintf(msg, sizeof(msg), "Line %d: The quick brown fox jumps over the lazy dog. ", j);
      msg[sizeof(msg) - 1] = '\0';
      if (!send_to_desc(d, msg, 0))
        return false;
      if (!send_to_desc(d, "Lorem ipsum dolor sit amet, consectetur adipiscing elit.\r\n", 0))
        return false;
    }
    if (!send_to_desc(d, "Test complete!\r\n", 0))
      return false;
    return true;
  }
  if (strcmp(line, "stats") == 0)
  {
    char msg[512];
    snprintf(msg, sizeof(msg), "Bytes out: %-9zu (uncompressible)\r\n", d->uncompressed_out);
    msg[sizeof(msg) - 1] = '\0';
    if (!send_to_desc(d, msg, 0))
      return false;
    if (d->mccp4.zstd_stream)
      if (!send_to_desc(d, "MCCP4: ENABLED\r\n", 0))
        return false;
    if (d->mccp4.mccp4_bytes_in > 0)
    {
      snprintf(
          msg, sizeof(msg),
          " MCCP4 statistics:\r\n"
          "  Bytes in:          %-9zu (compressible)\r\n"
          "  Bytes out:         %-9zu (compressed)\r\n"
          "  Compression ratio: %+-6.2f%%\r\n"
          "  Buffer capacity:   %-9zu bytes\r\n"
          "  Buffer pending:    %-9zu bytes\r\n",
          d->mccp4.mccp4_bytes_in, d->mccp4.mccp4_bytes_out, DRATIO(d->mccp4.mccp4_bytes_out, d->mccp4.mccp4_bytes_in),
          d->mccp4.zstd_outcap, d->mccp4.zstd_outpos
      );
      msg[sizeof(msg) - 1] = '\0';
      if (!send_to_desc(d, msg, 0))
        return false;
    }
    if (d->mccp2.zstate)
      if (!send_to_desc(d, "MCCP2: ENABLED\r\n", 0))
        return false;
    if (d->mccp2.mccp2_bytes_in > 0)
    {
      snprintf(
          msg, sizeof(msg),
          " MCCP2 statistics:\r\n"
          "  Bytes in:          %-9zu (compressible)\r\n"
          "  Bytes out:         %-9zu (compressed)\r\n"
          "  Compression ratio: %+-6.2f%%\r\n",
          d->mccp2.mccp2_bytes_in, d->mccp2.mccp2_bytes_out, DRATIO(d->mccp2.mccp2_bytes_out, d->mccp2.mccp2_bytes_in)
      );
      msg[sizeof(msg) - 1] = '\0';
      if (!send_to_desc(d, msg, 0))
        return false;
    }
    if (!d->mccp4.zstd_stream && !d->mccp2.zstate)
      if (!send_to_desc(d, "Compression: DISABLED\r\n", 0))
        return false;
    return true;
  }
  if (strcmp(line, "off") == 0)
  {
    if (d->mccp4.zstd_stream)
    {
      if (!send_to_desc(d, "MCCP4 Compression enabled, disabling...\r\n", 0))
        return false;
      if (!end_mccp4_compression(d))
        return false;
      if (!send_to_desc(d, "MCCP4 Compression disabled.\r\n", 0))
        return false;
    }
    else if (d->mccp2.zstate)
    {
      if (!send_to_desc(d, "MCCP2 Compression enabled, disabling...\r\n", 0))
        return false;
      if (!end_mccp2_compression(d))
        return false;
      if (!send_to_desc(d, "MCCP2 Compression disabled.\r\n", 0))
        return false;
    }
    else
    {
      if (!send_to_desc(d, "No compression is enabled.\r\n", 0))
        return false;
    }
    return true;
  }
  // Unknown command.
  char msg[128 + BUFFER_SIZE];
  snprintf(msg, sizeof(msg), "You said: %s\r\n", line);
  msg[sizeof(msg) - 1] = '\0';
  if (!send_to_desc(d, msg, 0))
    return false;
  return true;
}

/* Handle client connection */
void handle_client(int client_socket, struct sockaddr_in6 client_addr)
{
  char client_inet[INET6_ADDRSTRLEN + 1];
  if (!inet_ntop(AF_INET6, &client_addr.sin6_addr, client_inet, sizeof(client_inet)))
  {
    printf(
        "Error inet_ntop: %s\n", errno == EAFNOSUPPORT ? "EAFNOSUPPORT"
                                 : errno == ENOSPC     ? "ENOSPC"
                                                       : strerror(errno)
    );
    snprintf(client_inet, sizeof(client_inet), "unknown");
  }
  client_inet[sizeof(client_inet) - 1] = '\0';
  char client_name[512];
  snprintf(client_name, sizeof(client_name), "%d[%s:%d]", getpid(), client_inet, ntohs(client_addr.sin6_port));
  client_name[sizeof(client_name) - 1] = '\0';
  printf("SERVER: handle_client %s ...\n", client_name);
  descriptor_t d = {0};
  d.connection.socket = client_socket;
  memcpy(d.connection.inet_name, client_name, strlen(client_name) + 1);
  d.connection.inet_name[sizeof(d.connection.inet_name) - 1] = '\0';

  static unsigned char iac_will_mccp4[] = {IAC, WILL, TELOPT_COMPRESS4};
  static unsigned char iac_will_mccp2[] = {IAC, WILL, TELOPT_COMPRESS2};
  if (!send_to_desc(&d, (char *)iac_will_mccp4, sizeof(iac_will_mccp4)))
    goto cleanup;
  if (!send_to_desc(&d, (char *)iac_will_mccp2, sizeof(iac_will_mccp2)))
    goto cleanup;
  printf("Server: Sent IAC WILL COMPRESS4 and IAC WILL COMPRESS2\n");

  char greeting[1024];
  snprintf(greeting, sizeof(greeting), "%s\r\nYou are %s\r\n", initial_greeting, client_name);
  greeting[sizeof(greeting) - 1] = '\0';

  if (!send_to_desc(&d, greeting, 0))
    goto cleanup;
  if (!send_to_desc(&d, help_page, 0))
    goto cleanup;

  /* Send prompt */
  if (!send_to_desc(&d, "> ", 0))
    goto cleanup;

  // Actually send data over the wire:
  printf("DEBUG: sending banner, will, etc to %s\n", client_name);
  if (!flush_desc(&d))
    goto cleanup;

  /* Main loop */
  unsigned char buffer[BUFFER_SIZE];
  char tmp[BUFFER_SIZE];

  bool shown_prompt = false;
  while (1)
  {
    int n = recv(client_socket, buffer, sizeof(buffer), 0);
    if (n <= 0)
      break;

    for (int i = 0; i < n; i++)
    {
      switch (d.telnet.state)
      {
      case TELNET_STATE_TEXT:
        if (buffer[i] == IAC)
        {
          d.telnet.state = TELNET_STATE_IAC;
        }
        else
        {
          if (buffer[i] == '\r')
          {
            // Ignore this.
          }
          else if (buffer[i] == '\n')
          {
            // Process command.
            if (!process_command(&d, (char *)d.input.ibuf, d.input.ipos))
              goto cleanup;
            /* Send prompt */
            if (!send_to_desc(&d, "> ", 0))
              goto cleanup;
            shown_prompt = true;
            // Send all data:
            printf("DEBUG: flushing after prompt after input from %s ...\n", client_name);
            if (!flush_desc(&d))
              goto cleanup;
            d.input.ipos = 0;
            d.input.ibuf[0] = '\0';
          }
          else
          {
            /* Normal text byte */
            if (d.input.ipos < sizeof(d.input.ibuf) - 1)
            {
              d.input.ibuf[d.input.ipos++] = buffer[i];
            }
            else
            {
              printf("WARN: %s: input overflow with byte %d\n", client_name, buffer[i]);
            }
            d.input.ibuf[d.input.ipos] = '\0';
          }
        }
        break;
      case TELNET_STATE_IAC:
        if (buffer[i] == IAC)
        {
          /* Escaped IAC, treat as data */
          if (d.input.ipos < sizeof(d.input.ibuf) - 1)
          {
            d.input.ibuf[d.input.ipos++] = (unsigned char)IAC;
            d.input.ibuf[d.input.ipos] = '\0';
          }
          d.telnet.state = TELNET_STATE_TEXT;
        }
        else if (buffer[i] == DO)
        {
          d.telnet.state = TELNET_STATE_IAC_DO;
        }
        else if (buffer[i] == DONT)
        {
          d.telnet.state = TELNET_STATE_IAC_DONT;
        }
        else if (buffer[i] == WILL)
        {
          d.telnet.state = TELNET_STATE_IAC_WILL;
        }
        else if (buffer[i] == WONT)
        {
          d.telnet.state = TELNET_STATE_IAC_WONT;
        }
        else if (buffer[i] == SB)
        {
          d.telnet.state = TELNET_STATE_IAC_SB;
        }
        else
        {
          printf("IAC followed by unhandled byte %d. Back to text for %s !\n", buffer[i], client_name);
          snprintf(tmp, sizeof(tmp), "[Got: IAC %d, unsupported]\r\n", buffer[i]);
          tmp[sizeof(tmp) - 1] = '\0';
          if (!send_to_desc(&d, tmp, 0))
            goto cleanup;
          d.telnet.state = TELNET_STATE_TEXT;
        }
        break;
      case TELNET_STATE_IAC_DONT:
        printf("Client %s says DONT %d.\n", client_name, buffer[i]);
        snprintf(tmp, sizeof(tmp), "[Got: IAC DONT %d]\r\n", buffer[i]);
        tmp[sizeof(tmp) - 1] = '\0';
        if (!send_to_desc(&d, tmp, 0))
          goto cleanup;
        if (buffer[i] == TELOPT_COMPRESS4)
        {
          // Reply WONT?
          if (d.mccp4.zstd_stream)
          {
            printf("Client %s says DONT COMPRESS4=%d, but it's enabled. Ending ...!\n", client_name, buffer[i]);
            if (!end_mccp4_compression(&d))
              goto cleanup;
          }
        }
        else if (buffer[i] == TELOPT_COMPRESS2)
        {
          // Reply WONT?
          if (d.mccp2.zstate)
          {
            printf("Client %s says DONT COMPRESS2=%d, but it's enabled. Ending ...!\n", client_name, buffer[i]);
            if (!end_mccp2_compression(&d))
              goto cleanup;
          }
        }
        else
        {
          printf("Client %s says DONT %d. No special handling ...\n", client_name, buffer[i]);
        }
        d.telnet.state = TELNET_STATE_TEXT;
        break;
      case TELNET_STATE_IAC_DO:
        printf("Client %s says DO %d ...\n", client_name, buffer[i]);
        snprintf(tmp, sizeof(tmp), "[Got: IAC DO %d]\r\n", buffer[i]);
        tmp[sizeof(tmp) - 1] = '\0';
        if (!send_to_desc(&d, tmp, 0))
          goto cleanup;
        if (buffer[i] == TELOPT_COMPRESS4)
        {
          printf("Client %s says DO COMPRESS4=%d. Awaiting IAC SB ...)\n", client_name, buffer[i]);
          d.mccp4.can_mccp4 = true;
        }
        else if (buffer[i] == TELOPT_COMPRESS2)
        {
          printf("Client %s says DO COMPRESS2=%d.\n", client_name, buffer[i]);
          d.mccp2.can_mccp2 = true;
          if (d.mccp2.zstate)
          {
            printf("Client %s: can_mccp2, but already enabled.\n", client_name);
          }
          else if (d.mccp4.zstd_stream)
          {
            printf("Client %s: can_mccp2, but MCCP4 already enabled.\n", client_name);
          }
          else
          {
            if (d.mccp4.can_mccp4)
              printf("Client %s: can_mccp4, so not starting MCCP2 yet.\n", client_name);
            else
            {
              printf("Client %s: !can_mccp4, so starting MCCP2 now!\n", client_name);
              if (!start_mccp2_compression(&d, -1))
                goto cleanup;
              if (!send_to_desc(&d, "[Enabled MCCP2 compression]\r\n", 0))
                goto cleanup;
            }
          }
        }
        else
        {
          printf("Client %s says DO %d. Ignoring ...\n", client_name, buffer[i]);
        }
        d.telnet.state = TELNET_STATE_TEXT;
        break;
      case TELNET_STATE_IAC_WONT:
        printf("Client %s says WONT %d. Ignored ...\n", client_name, buffer[i]);
        snprintf(tmp, sizeof(tmp), "[Got: IAC WONT %d]\r\n", buffer[i]);
        tmp[sizeof(tmp) - 1] = '\0';
        if (!send_to_desc(&d, tmp, 0))
          goto cleanup;
        d.telnet.state = TELNET_STATE_TEXT;
        break;
      case TELNET_STATE_IAC_WILL:
        printf("Client %s says WILL %d. Ignored ...\n", client_name, buffer[i]);
        snprintf(tmp, sizeof(tmp), "[Got: IAC WILL %d]\r\n", buffer[i]);
        tmp[sizeof(tmp) - 1] = '\0';
        if (!send_to_desc(&d, tmp, 0))
          goto cleanup;
        d.telnet.state = TELNET_STATE_TEXT;
        break;
      case TELNET_STATE_IAC_SB:
        if (buffer[i] == IAC)
        {
          d.telnet.state = TELNET_STATE_IAC_SB_IAC;
        }
        else
        {
          if (d.telnet.sb_pos < sizeof(d.telnet.sb_data) - 1)
          {
            d.telnet.sb_data[d.telnet.sb_pos++] = buffer[i];
          }
        }
        break;
      case TELNET_STATE_IAC_SB_IAC:
        if (buffer[i] == SE)
        {
          /* End of subnegotiation */
          printf("DEBUG: Received IAC SB ... IAC SE from %s, length=%zu\n", client_name, d.telnet.sb_pos);
          if (d.telnet.sb_pos > 0)
          {
            snprintf(tmp, sizeof(tmp), "[Got: IAC SB %d ... IAC SE]\r\n", d.telnet.sb_data[0]);
            tmp[sizeof(tmp) - 1] = '\0';
            if (!send_to_desc(&d, tmp, 0))
              goto cleanup;
            if (d.telnet.sb_data[0] == TELOPT_COMPRESS4)
            {
              if (d.telnet.sb_pos > 1)
              {
                if (d.telnet.sb_data[1] == MCCP4_ACCEPT_ENCODING)
                {
                  int enc_len = d.telnet.sb_pos - 2;
                  if (enc_len > 0)
                  {
                    char encodings[256] = {0};
                    memcpy(encodings, &d.telnet.sb_data[2], enc_len < 255 ? enc_len : 255);
                    printf(
                        "DEBUG: ACCEPT_ENCODING received from descriptor %d %s, encodings len %d '%s'\n",
                        d.connection.socket, client_name, enc_len, encodings
                    );
                    // Did it accept "zstd"?
                    if (enc_len >= 4 && memcmp(encodings, "zstd", 4) == 0)
                    {
                      printf("Server: Client %s accepted 'zstd' encoding. Starting compression.\n", client_name);
                      snprintf(
                          tmp, sizeof(tmp),
                          "[Got: IAC SB MCCP4 ACCEPT_ENCODING %s IAC SE, starting MCCP4 compression ...]\r\n", encodings
                      );
                      tmp[sizeof(tmp) - 1] = '\0';
                      if (!send_to_desc(&d, tmp, 0))
                        goto cleanup;
                      if (!start_mccp4_compression(&d, 9))
                        goto cleanup;
                      if (!send_to_desc(&d, "[Enabled MCCP4 compression]\r\n", 0))
                        goto cleanup;
                    }
                    else
                    {
                      printf("Server: Client %s did not accept 'zstd' encoding!\n", client_name);
                      // FIXME: maybe it accepts deflate?
                      snprintf(
                          tmp, sizeof(tmp), "[Got: IAC SB MCCP4 ACCEPT_ENCODING %s IAC SE, unrecognised]\r\n", encodings
                      );
                      tmp[sizeof(tmp) - 1] = '\0';
                      if (!send_to_desc(&d, tmp, 0))
                        goto cleanup;
                    }
                  }
                  else
                  {
                    printf("Server: Received ACCEPT_ENCODING (empty) from %s\n", client_name);
                    snprintf(tmp, sizeof(tmp), "[Got: IAC SB MCCP4 ACCEPT_ENCODING IAC SE, empty]\r\n");
                    tmp[sizeof(tmp) - 1] = '\0';
                    if (!send_to_desc(&d, tmp, 0))
                      goto cleanup;
                  }
                }
                else
                {
                  printf("DEBUG: IAC SB MCCP4 not followed by ACCEPT_ENCODING for %s!\n", client_name);
                  snprintf(tmp, sizeof(tmp), "[Got: IAC SB MCCP4 IAC SE, unsupported]\r\n");
                  tmp[sizeof(tmp) - 1] = '\0';
                  if (!send_to_desc(&d, tmp, 0))
                    goto cleanup;
                }
              }
            }
            else
            {
              printf("DEBUG: unhandled IAC SB %d from %s!\n", d.telnet.sb_data[0], client_name);
              snprintf(
                  tmp, sizeof(tmp), "[Got: IAC SB %d IAC SE, with %zu payload, unsupported]\r\n", d.telnet.sb_data[0],
                  d.telnet.sb_pos
              );
              tmp[sizeof(tmp) - 1] = '\0';
              if (!send_to_desc(&d, tmp, 0))
                goto cleanup;
            }
          }
          else
          {
            printf("DEBUG: unhandled IAC SB with no data from %s!\n", client_name);
            snprintf(tmp, sizeof(tmp), "[Got: IAC SB IAC SE, with 0 payload, unsupported]\r\n");
            tmp[sizeof(tmp) - 1] = '\0';
            if (!send_to_desc(&d, tmp, 0))
              goto cleanup;
          }
          d.telnet.sb_pos = 0;
          d.telnet.state = TELNET_STATE_TEXT;
        }
        else if (buffer[i] == IAC)
        {
          /* Escaped IAC within SB */
          if (d.telnet.sb_pos < sizeof(d.telnet.sb_data) - 1)
          {
            d.telnet.sb_data[d.telnet.sb_pos++] = IAC;
          }
          d.telnet.state = TELNET_STATE_IAC_SB;
        }
        else
        {
          /* Protocol error, but recover */
          printf("Protocol error in SB: expected SE or IAC, got %d. Back to text for %s.\n", buffer[i], client_name);
          snprintf(tmp, sizeof(tmp), "[Got: IAC SB ... IAC followed by neither IAC nor SE, unsupported]\r\n");
          tmp[sizeof(tmp) - 1] = '\0';
          if (!send_to_desc(&d, tmp, 0))
            goto cleanup;
          d.telnet.sb_pos = 0;
          d.telnet.state = TELNET_STATE_TEXT;
        }
        break;
      }
    }
    // Actually send bytes, if any are pending:
    if (d.output.opos > 0)
    {
      if (!shown_prompt)
        if (!send_to_desc(&d, "> ", 0))
          goto cleanup;
      printf("DEBUG: Pending %9zu bytes, flushing %s ...\n", d.output.opos, client_name);
      if (!flush_desc(&d))
        goto cleanup;
      shown_prompt = false;
    }
  }

cleanup:
  printf("Server: Client disconnection for %s ...\n", client_name);

  /* Clean up compressions, if any */

  if (d.mccp4.zstd_stream)
    if (!end_mccp4_compression(&d))
      printf("BUG? %s: could not end MCCP4 compression!\n", client_name);
  if (d.mccp2.zstate)
    if (!end_mccp2_compression(&d))
      printf("BUG? %s: could not end MCCP2 compression!\n", client_name);

  printf("DEBUG: For %s: Overall: %9zu uncompressible", client_name, d.uncompressed_out);

  if (d.mccp4.mccp4_bytes_in > 0)
    printf(
        ", MCCP4: %9zu for %9zu len, %+8.2f%% ratio", d.mccp4.mccp4_bytes_out, d.mccp4.mccp4_bytes_in,
        DRATIO(d.mccp4.mccp4_bytes_out, d.mccp4.mccp4_bytes_in)
    );
  else
    printf(", no MCCP4 data");

  if (d.mccp2.mccp2_bytes_in > 0)
    printf(
        ", MCCP2: %9zu for %9zu len, %+8.2f%% ratio", d.mccp2.mccp2_bytes_out, d.mccp2.mccp2_bytes_in,
        DRATIO(d.mccp2.mccp2_bytes_out, d.mccp2.mccp2_bytes_in)
    );
  else
    printf(", no MCCP2 data");

  printf("\n");

  printf("Server: Client socket closed for %s.\n", client_name);
  close(client_socket);
}

int main(int argc, char *argv[])
{
  int port = DEFAULT_PORT;
  if (argc > 1)
    port = atoi(argv[1]);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  /* Create socket */
  int server_socket = socket(AF_INET6, SOCK_STREAM, 0);
  if (server_socket < 0)
  {
    perror("socket");
    return 1;
  }

  /* Allow reuse */
  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  /* Bind */
  struct sockaddr_in6 addr = {0};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);

  if (bind(server_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
  {
    perror("bind");
    close(server_socket);
    return 1;
  }

  /* Listen */
  if (listen(server_socket, 1) < 0)
  {
    perror("listen");
    close(server_socket);
    return 1;
  }

  printf("MCCP4 Test Server listening on port %d\n", port);
  printf("Commands: test, stats, compress, quit\n");
  printf("Debug output enabled - you'll see all negotiation details\n\n");

  /* Accept connections */
  while (1)
  {
    struct sockaddr_in6 client_addr;
    socklen_t client_len = sizeof(client_addr);
    memset(&client_addr, 0, client_len);

    int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
    if (client_socket < 0)
    {
      perror("accept");
      continue;
    }

    int pid = fork();
    if (pid == 0)
    {
      close(server_socket);
      handle_client(client_socket, client_addr);
      printf("Child %d: Exiting ...\n", getpid());
      exit(0);
    }
    close(client_socket);
    char client_inet[INET6_ADDRSTRLEN + 1];
    if (!inet_ntop(AF_INET6, &client_addr.sin6_addr, client_inet, sizeof(client_inet)))
    {
      snprintf(client_inet, sizeof(client_inet), "unknown");
    }
    client_inet[sizeof(client_inet) - 1] = '\0';
    printf("SERVER: created child %d for client %s:%d\n", pid, client_inet, ntohs(client_addr.sin6_port));
  }

  close(server_socket);
  return 0;
}
