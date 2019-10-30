# libblit

libblit is a small 2D graphics library. It aims to provide an API
along the lines of the [X11 render extension][x11-render], Plan
9's [libdraw][plan9-libdraw], and Go's [image/draw][go-image/draw]
package. Another goal is to provide a common interface that can be used
with a variety of windowing systems and rendering systems.

Here are some planned rendering and display mechanisms:

- Rendering to X11 pixmap with X11 render extension, presented with X11
  present extension.
- Rendering and presenting with Vulkan to `VkSurfaceKHR` (X11 or Wayland).
- Rendering to SHM buffer with pixman, exported to X11 pixmap with X11
  SHM extension, presented with X11 present extension.
- Rendering to SHM buffer with pixman, exported to `wl_buffer` with
  `wl_shm`, attached to `wl_surface`.
- Rendering to mmapped DRM dumb buffer with pixman, presented directly
  to display with KMS.
- Rendering to `VkImage` imported from `gbm_bo` created for scanout with
  GBM, exported to DMA-BUF fd, presented directly to display with KMS.

## Current status

libblit is currently in the design/experimentation phase, so none of
the API is final and may change dramatically without notice.

### Things to figure out

- Importing/exporting buffers (DMA-BUF, SHM).
- Synchronization, both internally and DRM syncobj for imported/exported
  buffers.
- Format modifier for image creation.

[x11-render]: https://gitlab.freedesktop.org/xorg/proto/xorgproto/raw/master/renderproto.txt
[plan9-libdraw]: http://man.cat-v.org/plan_9/2/draw
[go-image/draw]: https://golang.org/pkg/image/draw/
