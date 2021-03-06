// MIT License
// Copyright (c) Tyler Graff 2017-2020
// tagraff@gmail.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#include "framecap.h"

struct Framecap {
  int       fd;         // Device handle
  uint32_t  bufcnt;     // # of buffers
  uint8_t **fbuf;       // frame buffers
};


// Wrap ioctl() to spin on EINTR
static int eintr_ioctl(int fd, int req, void* arg)
{
  struct timespec poll_time;
  int r;
  while ((r = ioctl(fd, req, arg))) {
    if(r == -1 && (EINTR != errno && EAGAIN != errno))
      break;

    // 10 milliseconds
    poll_time.tv_sec = 0;
    poll_time.tv_nsec = 10000000;
    nanosleep(&poll_time, NULL);
  }
  return r;
}

// Create a new context to capture frames from <fname>.
// Returns NULL on error.
Framecap * framecap_new(const char *device, uint32_t bufcnt)
{
  struct v4l2_capability     cap;
  struct v4l2_cropcap        cropcap;
  struct v4l2_crop           crop;
  struct v4l2_format         vfmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer         buf;
  enum   v4l2_buf_type       type;
  uint32_t  ii;
  Framecap  *ctx;

  ctx = malloc(sizeof(Framecap));
  if (!ctx)
    return NULL;
  memset(ctx, 0, sizeof(Framecap));
  ctx->bufcnt = bufcnt;
  ctx->fbuf = malloc(sizeof(uint8_t*) * ctx->bufcnt);

  ctx->fd = open(device, O_RDWR | O_NONBLOCK, 0);
  if (ctx->fd < 0)
    {fprintf(stderr, "ERROR: Cannot open device"); return NULL;}

  // Determine if fd is a V4L2 Device
  if (0 != eintr_ioctl(ctx->fd, VIDIOC_QUERYCAP, &cap))
    {fprintf(stderr, "ERROR: Not v4l2 compatible"); return NULL;}

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {fprintf(stderr, "ERROR: Capture not supported"); return NULL;}

  if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {fprintf(stderr, "ERROR: Streaming IO Not Supported"); return NULL;}

  // Set crop, ignore ioctl errors
  cropcap = (struct v4l2_cropcap){0};
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  eintr_ioctl(ctx->fd, VIDIOC_CROPCAP, &cropcap);

  crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  crop.c    = cropcap.defrect; // reset to default
  eintr_ioctl(ctx->fd, VIDIOC_S_CROP, &crop);

  // Preserve original settings as set by v4l2-ctl for example
  vfmt = (struct v4l2_format){0};
  vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == eintr_ioctl(ctx->fd, VIDIOC_G_FMT, &vfmt))
   {fprintf(stderr, "ERROR: VIDIOC_G_FMT"); return NULL;}

#if(0)
  // Buggy driver paranoia
  min = vfmt.fmt.pix.width * 2;
  if(vfmt.fmt.pix.bytesperline < min)
    vfmt.fmt.pix.bytesperline = min;

  min = vfmt.fmt.pix.bytesperline * vfmt.fmt.pix.height;
  if(vfmt.fmt.pix.sizeimage < min)
    vfmt.fmt.pix.sizeimage = min;

  // Save off image parameters to pass to callback
  w_pix  = vfmt.fmt.pix.width;
  h_pix  = vfmt.fmt.pix.height;
  img_fmt = vfmt.fmt.pix.pixelformat; // YUYV422, MJPEG, etc
#endif

  // Request memory-mapped buffers
  req = (struct v4l2_requestbuffers){0};
  req.count  = ctx->bufcnt;
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if(-1 == eintr_ioctl(ctx->fd, VIDIOC_REQBUFS, &req))
    {fprintf(stderr, "ERROR: Device does not support mmap"); return NULL;}
  if(req.count != ctx->bufcnt)
    {fprintf(stderr, "ERROR: Device buffer count mismatch"); return NULL;}

  // mmap() the buffers into userspace memory
  for (ii = 0 ; ii < ctx->bufcnt; ii++)
  {
    buf = (struct v4l2_buffer){0};
    buf.type    = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory  = V4L2_MEMORY_MMAP;
    buf.index   = ii;
    if(-1 == eintr_ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf))
      {fprintf(stderr, "ERROR: VIDIOC_QUERYBUF"); return NULL;}

    ctx->fbuf[ii] = mmap(NULL, buf.length,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         ctx->fd, buf.m.offset);
    if(MAP_FAILED == ctx->fbuf[ii])
      {fprintf(stderr, "ERROR: Failed to map device frame buffers"); return NULL;}

    // Set up buffers
    buf = (struct v4l2_buffer){0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = ii;
    if (-1 == eintr_ioctl(ctx->fd, VIDIOC_QBUF, &buf))
      {fprintf(stderr, "ERROR: VIDIOC_QBUF"); return NULL;}
  }

  // Start capturing
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == eintr_ioctl(ctx->fd, VIDIOC_STREAMON, &type))
    {fprintf(stderr, "ERROR: VIDIOC_STREAMON"); return NULL;}

  return ctx;
}


// Free a context to capture frames from <fname>.
// Returns NULL on error.
int framecap_free(Framecap *ctx)
{
  struct v4l2_buffer  buf;
  enum v4l2_buf_type  type;
  uint32_t            ii;

  // Stop capturing
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  eintr_ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

  // un-mmap() buffers
  for (ii = 0 ; ii < ctx->bufcnt; ii++)
  {
    buf = (struct v4l2_buffer){0};
    buf.type    = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory  = V4L2_MEMORY_MMAP;
    buf.index   = ii;
    if(-1 == eintr_ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf))
      {fprintf(stderr, "ERROR: VIDIOC_QUERYBUF");}

    munmap(ctx->fbuf[buf.index], buf.length);
  }

  // Close v4l2 device
  close(ctx->fd);
  free(ctx->fbuf);
  free(ctx);
  return 0;
}


// Returns a pointer to a captured frame and its meta-data. NOT thread-safe.
uint8_t * framecap_next(Framecap *ctx,
                       uint32_t *l, uint32_t *w, uint32_t *h, uint32_t *ffmt) {
  struct v4l2_buffer buf;
  struct v4l2_format vfmt;
  struct timeval     timeout;
  fd_set fds;
  int    r;

  FD_ZERO(&fds);
  FD_SET(ctx->fd, &fds);

  timeout.tv_sec  = 10;
  timeout.tv_usec = 0;

  for (;r < 1;) {
    r = select(ctx->fd + 1, &fds, NULL, NULL, &timeout);
    if (0 == r)
      {fprintf(stderr, "ERROR: select timeout"); return NULL;}
    if (-1 == r && EINTR != errno && EAGAIN != errno)
      {fprintf(stderr, "ERROR: select() returned %d", r); return NULL;}
  }

  // Preserve original settings as set by v4l2-ctl for example
  vfmt = (struct v4l2_format){0};
  vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == eintr_ioctl(ctx->fd, VIDIOC_G_FMT, &vfmt))
    {fprintf(stderr, "ERROR: VIDIOC_G_FMT"); return NULL;}

#if(0)
  // Buggy driver paranoia
  min = vfmt.fmt.pix.width * 2;
  if(vfmt.fmt.pix.bytesperline < min)
    vfmt.fmt.pix.bytesperline = min;

  min = vfmt.fmt.pix.bytesperline * vfmt.fmt.pix.height;
  if(vfmt.fmt.pix.sizeimage < min)
    vfmt.fmt.pix.sizeimage = min;
#endif

  buf = (struct v4l2_buffer){0};
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (-1 == eintr_ioctl(ctx->fd, VIDIOC_DQBUF, &buf)) {
    fprintf(stderr, "ERROR: VIDIOC_DQBUF");
    return NULL;
  }

  if(buf.index > ctx->bufcnt)
    {fprintf(stderr, "ERROR: buffer index out of bounds"); return NULL;}

  // total bytes
  if (l)
    *l = buf.bytesused;

  // pixel width
  if (w)
    *w  = vfmt.fmt.pix.width;

  // pixel height
  if (h)
    *h  = vfmt.fmt.pix.height;

  // Format: YUYV422, MJPEG, etc
  // See V4L2 documentation for values
  if (ffmt)
    *ffmt = vfmt.fmt.pix.pixelformat;

  return ctx->fbuf[buf.index];
}

// It's OK to capture into this framebuffer now
int framecap_done(Framecap *ctx, uint8_t *frame) {
  struct v4l2_buffer buf;
  uint32_t ii;
  int      indx = -1;

  // find the buffer's index
  for (ii = 0 ; ii < ctx->bufcnt; ii++) {
    if (frame != ctx->fbuf[ii])
      continue;

    indx = ii;
    break;
  }

  if (indx < 0)
    return -1;

  buf = (struct v4l2_buffer){0};
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index  = indx;

  // Tell kernel it's ok to overwrite this frame
  if (-1 == eintr_ioctl(ctx->fd, VIDIOC_QBUF, &buf))
    {fprintf(stderr, "ERROR:  VIDIOC_QBUF"); return -1;}

  return 0;
}
