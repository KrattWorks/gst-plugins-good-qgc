/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include <gst/video/video.h>
#include "qtitem.h"
#include "gstqsgtexture.h"
#include "gstqtglutility.h"

#include <QtCore/QRunnable>
#include <QtCore/QMutexLocker>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGSimpleTextureNode>
#include <thread>
#include <mutex> // lock_guard

#ifdef HAVE_QT_QPA_HEADER
#include <qpa/qplatformnativeinterface.h>
#endif

/**
 * SECTION:gtkgstglwidget
 * @short_description: a #GtkGLArea that renders GStreamer video #GstBuffers
 * @see_also: #GtkGLArea, #GstBuffer
 *
 * #QtGLVideoItem is an #QQuickItem that renders GStreamer video buffers.
 */

#define GST_CAT_DEFAULT qt_item_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define DEFAULT_FORCE_ASPECT_RATIO  TRUE
#define DEFAULT_PAR_N               0
#define DEFAULT_PAR_D               1

enum
{
  PROP_0,
  PROP_FORCE_ASPECT_RATIO,
  PROP_PIXEL_ASPECT_RATIO,
};

struct SmartGMutex
{
  GMutex mtx;
  SmartGMutex() { g_mutex_init(&mtx); }
  ~SmartGMutex() { g_mutex_clear(&mtx); }
  void lock() { g_mutex_lock(&mtx); }
  void unlock() { g_mutex_unlock(&mtx); }
  SmartGMutex(const SmartGMutex&) = delete;
  SmartGMutex& operator=(const SmartGMutex&) = delete;
  SmartGMutex(SmartGMutex&&) = delete;
  SmartGMutex& operator=(SmartGMutex&&) = delete;
};

struct _QtGLVideoItemPrivate
{
  SmartGMutex lock;
  SmartGMutex buffer_lock;

  /* properties */
  gboolean force_aspect_ratio;
  gint par_n, par_d;

  gint display_width;
  gint display_height;

  gboolean negotiated;
  GstBuffer *front_buffer;
  GstBuffer *back_buffer;
  gboolean waiting_on_render;
  GstCaps *caps;
  GstVideoInfo v_info;

  gboolean initted;
  GstGLDisplay *display;
  QOpenGLContext *qt_context;
  GstGLContext *other_context;
  GstGLContext *context;
};

class InitializeSceneGraph : public QRunnable
{
public:
  InitializeSceneGraph(QtGLVideoItem *item);
  void run();

private:
  QtGLVideoItem *item_;
};

InitializeSceneGraph::InitializeSceneGraph(QtGLVideoItem *item) :
  item_(item)
{
}

void InitializeSceneGraph::run()
{
  item_->onSceneGraphInitialized();
}

QtGLVideoItem::QtGLVideoItem()
{
  static gsize _debug;

  if (g_once_init_enter (&_debug)) {
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "qtglwidget", 0, "Qt GL Widget");
    g_once_init_leave (&_debug, 1);
  }
  m_openGlContextInitialized = false;
  setFlag(QQuickItem::ItemHasContents, true);

  priv = QSharedPointer<QtGLVideoItemPrivate>(new QtGLVideoItemPrivate{});
  priv->force_aspect_ratio = DEFAULT_FORCE_ASPECT_RATIO;
  priv->par_n = DEFAULT_PAR_N;
  priv->par_d = DEFAULT_PAR_D;
  priv->display = gst_qt_get_gl_display();

  connect(this, SIGNAL(windowChanged(QQuickWindow*)), this,
          SLOT(handleWindowChanged(QQuickWindow*)));

  proxy = QSharedPointer<QtGLVideoItemInterface>(new QtGLVideoItemInterface(this));

  GST_DEBUG ("%p init Qt Video Item", this);
}

QtGLVideoItem::~QtGLVideoItem()
{
  /* Before destroying the priv info, make sure
   * no qmlglsink's will call in again, and that
   * any ongoing calls are done by invalidating the proxy
   * pointer */
  GST_INFO("Destroying QtGLVideoItem and invalidating the proxy");
  proxy.clear(); // call QtGLVideoItemInterface destructor

  if (priv->context) gst_object_unref(priv->context);
  if (priv->other_context) gst_object_unref(priv->other_context);
  if (priv->display) gst_object_unref(priv->display);

  priv.clear(); // call QtGLVideoItemPrivate destructor
}

void QtGLVideoItem::setDAR(gint num, gint den)
{
  priv->par_n = num;
  priv->par_d = den;
}

void QtGLVideoItem::getDAR(gint * num, gint * den)
{
  if (num) *num = priv->par_n;
  if (den) *den = priv->par_d;
}

void QtGLVideoItem::setForceAspectRatio(bool force_aspect_ratio)
{
  priv->force_aspect_ratio = !!force_aspect_ratio;
}

bool QtGLVideoItem::getForceAspectRatio()
{
  return priv->force_aspect_ratio;
}

bool QtGLVideoItem::itemInitialized()
{
  return m_openGlContextInitialized;
}

QSGNode* QtGLVideoItem::updatePaintNode(QSGNode* oldNode,
                                        UpdatePaintNodeData* updatePaintNodeData)
{
  (void)updatePaintNodeData;
  if (!m_openGlContextInitialized) {
    return oldNode;
  }

  QSGSimpleTextureNode* texNode = static_cast<QSGSimpleTextureNode*>(oldNode);
  GstVideoRectangle src, dst, result;
  GstQSGTexture *tex;

  auto p = priv;
  std::lock_guard lock { p->lock };
  if (!p->caps) {
      p->waiting_on_render = false;
      return NULL;
  }

  {
    std::lock_guard buffer_lock { p->buffer_lock };
    std::swap(p->front_buffer, p->back_buffer);
  }

  gst_gl_context_activate(p->other_context, TRUE);
  if (!texNode) {
    texNode = new QSGSimpleTextureNode();
    texNode->setOwnsTexture(true);
    texNode->setTexture(new GstQSGTexture());
  }

  tex = static_cast<GstQSGTexture*>(texNode->texture());
  tex->setCaps(p->caps);
  tex->setBuffer(p->front_buffer);
  texNode->markDirty(QSGNode::DirtyMaterial);

  if (p->force_aspect_ratio) {
    src.w = p->display_width;
    src.h = p->display_height;

    dst.x = boundingRect().x();
    dst.y = boundingRect().y();
    dst.w = boundingRect().width();
    dst.h = boundingRect().height();

    gst_video_sink_center_rect(src, dst, &result, TRUE);
  } else {
    result.x = boundingRect().x();
    result.y = boundingRect().y();
    result.w = boundingRect().width();
    result.h = boundingRect().height();
  }

  texNode->setRect(QRectF(result.x, result.y, result.w, result.h));

  gst_gl_context_activate(p->other_context, FALSE);
  p->waiting_on_render = false;
  return texNode;
}

static void _reset(QtGLVideoItemPrivate* priv)
{
  {
    std::lock_guard buffer_lock { priv->buffer_lock };
    gst_buffer_replace(&priv->back_buffer, NULL);
    gst_buffer_replace(&priv->front_buffer, NULL);
  }
  gst_caps_replace(&priv->caps, NULL);
  priv->negotiated = FALSE;
  priv->initted = FALSE;
  priv->waiting_on_render = FALSE;
}

QtGLVideoItemInterface::~QtGLVideoItemInterface()
{
  invalidateRef();
}

void QtGLVideoItemInterface::invalidateRef()
{
  QMutexLocker locker(&lock);
  if (qt_item) {
    if (auto p = qt_item->priv) {
      std::lock_guard lock { p->lock };
      _reset(p.get());
    }
    qt_item = nullptr;
  }
}

void QtGLVideoItemInterface::setBuffer(GstBuffer * buffer)
{
  QMutexLocker locker(&lock);
  if (qt_item == NULL)
    return;

  auto p = qt_item->priv;
  if (!p) {
    GST_WARNING("QtGLVideoItemInterface destroyed, dropping");
    return;
  }
  if (!p->negotiated) {
    GST_WARNING("Got buffer on unnegotiated QtGLVideoItem. Dropping");
    return;
  }

  {
    std::lock_guard lock { p->buffer_lock };
    gst_buffer_replace(&p->back_buffer, buffer);
    p->waiting_on_render = true;
  }

  // queue buffer and wait until it finishes rendering
  // this wait is important when our device is too slow to render and decode frames
  // so that the pipeline can use its queue element to drop frames as required
  QMetaObject::invokeMethod(qt_item, "update", Qt::BlockingQueuedConnection);

  // wait up to 100ms for the rendering to finish
  auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
  while (p->waiting_on_render) {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    if (p->waiting_on_render && std::chrono::steady_clock::now() > until) {
      GST_WARNING ("Timed out waiting for rendering to finish");
      break;
    }
  }
}

/////////////////////////////////////////////////////////////////////

void
QtGLVideoItem::onSceneGraphInitialized ()
{
  void* wgl_device = nullptr;

#if GST_GL_HAVE_WINDOW_WIN32 && GST_GL_HAVE_PLATFORM_WGL && defined (HAVE_QT_WIN32) && defined (HAVE_QT_QPA_HEADER)
  HWND hWnd = nullptr;
  QWindow* window = this->window();
#endif

  GST_DEBUG ("scene graph initialization with Qt GL context %p",
      this->window()->openglContext ());

  auto p = priv;
  if (p->qt_context == this->window()->openglContext ())
    return;

  p->qt_context = this->window()->openglContext ();
  if (p->qt_context == NULL) {
    g_assert_not_reached ();
    return;
  }

#if GST_GL_HAVE_WINDOW_WIN32 && GST_GL_HAVE_PLATFORM_WGL && defined (HAVE_QT_WIN32) && defined (HAVE_QT_QPA_HEADER)
  if (window && window->handle()) {
      QPlatformNativeInterface* pni = QGuiApplication::platformNativeInterface();
      hWnd = static_cast<HWND>(pni->nativeResourceForWindow(QByteArrayLiteral("handle"), window));

      if (hWnd != nullptr) {
         wgl_device = GetWindowDC(hWnd);
      }
  }
#endif

  m_openGlContextInitialized = gst_qt_get_gl_wrapcontext (p->display,
      &p->other_context, &p->context, wgl_device);

#if GST_GL_HAVE_WINDOW_WIN32 && GST_GL_HAVE_PLATFORM_WGL && defined (HAVE_QT_WIN32) && defined (HAVE_QT_QPA_HEADER)
  if (wgl_device != nullptr) {
      ReleaseDC(hWnd, static_cast<HDC>(wgl_device));
      wgl_device = nullptr;
  }

  hWnd = nullptr;
  window = nullptr;
#endif

  GST_DEBUG ("%p created wrapped GL context %" GST_PTR_FORMAT, this,
      this->priv->other_context);

  emit itemInitializedChanged();
}

void
QtGLVideoItem::onSceneGraphInvalidated ()
{
  GST_FIXME ("%p scene graph invalidated", this);
}

gboolean
QtGLVideoItemInterface::initWinSys ()
{
  QMutexLocker locker(&lock);

  GError *error = NULL;
  if (qt_item == NULL)
    return FALSE;
  auto p = qt_item->priv;
  if (!p) return FALSE;

  std::lock_guard lock { p->lock };
  if (p->display && p->qt_context && p->other_context && p->context) {
    return TRUE; /* already have the necessary state */
  }

  if (!GST_IS_GL_DISPLAY (p->display)) {
    GST_ERROR("%p failed to retrieve display connection %" GST_PTR_FORMAT, qt_item, p->display);
    return FALSE;
  }

  if (!GST_IS_GL_CONTEXT (p->other_context)) {
    GST_ERROR("%p failed to retrieve wrapped context %" GST_PTR_FORMAT, qt_item, p->other_context);
    return FALSE;
  }

  p->context = gst_gl_context_new(p->display);
  if (!p->context) {
    return FALSE;
  }

  if (!gst_gl_context_create(p->context, p->other_context, &error)) {
    GST_ERROR("%s", error->message);
    return FALSE;
  }
  return TRUE;
}

void
QtGLVideoItem::handleWindowChanged(QQuickWindow *win)
{
  if (win) {
    if (win->isSceneGraphInitialized())
      win->scheduleRenderJob(new InitializeSceneGraph(this), QQuickWindow::BeforeSynchronizingStage);
    else
      connect(win, SIGNAL(sceneGraphInitialized()), this, SLOT(onSceneGraphInitialized()), Qt::DirectConnection);

    connect(win, SIGNAL(sceneGraphInvalidated()), this, SLOT(onSceneGraphInvalidated()), Qt::DirectConnection);
  } else {
    this->priv->qt_context = NULL;
  }
}

static gboolean _calculate_par(QtGLVideoItemPrivate* priv, GstVideoInfo * info)
{
  gboolean ok;
  gint width, height;
  gint par_n, par_d;
  gint display_par_n, display_par_d;
  guint display_ratio_num, display_ratio_den;

  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);

  par_n = GST_VIDEO_INFO_PAR_N (info);
  par_d = GST_VIDEO_INFO_PAR_D (info);

  if (!par_n)
    par_n = 1;

  /* get display's PAR */
  if (priv->par_n != 0 && priv->par_d != 0) {
    display_par_n = priv->par_n;
    display_par_d = priv->par_d;
  } else {
    display_par_n = 1;
    display_par_d = 1;
  }

  ok = gst_video_calculate_display_ratio (&display_ratio_num,
      &display_ratio_den, width, height, par_n, par_d, display_par_n,
      display_par_d);

  if (!ok)
    return FALSE;

  GST_LOG ("PAR: %u/%u DAR:%u/%u", par_n, par_d, display_par_n, display_par_d);

  if (height % display_ratio_den == 0) {
    GST_DEBUG ("keeping video height");
    priv->display_width = (guint)
        gst_util_uint64_scale_int (height, display_ratio_num,
        display_ratio_den);
    priv->display_height = height;
  } else if (width % display_ratio_num == 0) {
    GST_DEBUG ("keeping video width");
    priv->display_width = width;
    priv->display_height = (guint)
        gst_util_uint64_scale_int (width, display_ratio_den, display_ratio_num);
  } else {
    GST_DEBUG ("approximating while keeping video height");
    priv->display_width = (guint)
        gst_util_uint64_scale_int (height, display_ratio_num,
        display_ratio_den);
    priv->display_height = height;
  }
  GST_DEBUG ("scaling to %dx%d", priv->display_width, priv->display_height);

  return TRUE;
}

gboolean
QtGLVideoItemInterface::setCaps (GstCaps * caps)
{
  QMutexLocker locker(&lock);
  GstVideoInfo v_info;

  g_return_val_if_fail (GST_IS_CAPS (caps), FALSE);
  g_return_val_if_fail (gst_caps_is_fixed (caps), FALSE);

  if (qt_item == NULL)
    return FALSE;

  auto p = qt_item->priv;
  if (!p)
    return FALSE;
  if (p->caps && gst_caps_is_equal_fixed(p->caps, caps))
    return TRUE;

  if (!gst_video_info_from_caps(&v_info, caps))
    return FALSE;

  std::lock_guard lock { p->lock };
  _reset(p.get());
  gst_caps_replace(&p->caps, caps);
  if (!_calculate_par(p.get(), &v_info)) {
    return FALSE;
  }
  p->v_info = v_info;
  p->negotiated = TRUE;
  return TRUE;
}

GstGLContext* QtGLVideoItemInterface::getQtContext()
{
  QMutexLocker locker(&lock);
  if (!qt_item) return NULL;
  auto p = qt_item->priv;
  if (!p || !p->other_context) return NULL;
  return (GstGLContext*)gst_object_ref(p->other_context);
}

GstGLContext* QtGLVideoItemInterface::getContext()
{
  QMutexLocker locker(&lock);
  if (!qt_item) return NULL;
  auto p = qt_item->priv;
  if (!p || !p->context) return NULL;
  return (GstGLContext*)gst_object_ref(p->context);
}

GstGLDisplay* QtGLVideoItemInterface::getDisplay()
{
  QMutexLocker locker(&lock);
  if (!qt_item) return NULL;
  auto p = qt_item->priv;
  if (!p || !p->display) return NULL;
  return (GstGLDisplay*)gst_object_ref(p->display);
}

void QtGLVideoItemInterface::setDAR(gint num, gint den)
{
  QMutexLocker locker(&lock);
  if (qt_item) qt_item->setDAR(num, den);
}

void QtGLVideoItemInterface::getDAR(gint * num, gint * den)
{
  QMutexLocker locker(&lock);
  if (qt_item) qt_item->getDAR(num, den);
}

void QtGLVideoItemInterface::setForceAspectRatio(bool force_aspect_ratio)
{
  QMutexLocker locker(&lock);
  if (qt_item) qt_item->setForceAspectRatio(force_aspect_ratio);
}

bool QtGLVideoItemInterface::getForceAspectRatio()
{
  QMutexLocker locker(&lock);
  if (!qt_item) return FALSE;
  return qt_item->getForceAspectRatio();
}

