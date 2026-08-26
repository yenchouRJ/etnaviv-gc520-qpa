/*
 * qetnaviv2dwaylandtransport.h — Wayland client transport for the etnaviv2d QPA.
 *
 * Milestone B: fills the plane=wayland stub (isStub() seam in QEtnaviv2dScreen).
 *
 * Design (D11 / wayland/plan.md):
 *  - Owns all libwayland-client state: display, registry, compositor,
 *    xdg_wm_base, xdg_surface, xdg_toplevel, zwp_linux_dmabuf_v1.
 *  - Allocates a 3-deep ARGB8888/LINEAR BO ring whose BOs are:
 *      (a) rendered into by the GC520 blitter (EtnaBlittable / etna2d_blit),
 *      (b) exported as dmabuf via etna_bo_dmabuf(),
 *      (c) wrapped as persistent wl_buffer objects (created once, reused).
 *  - Frame lifecycle:
 *      BackingStore::flush() blits into the current free ring BO, then calls
 *      commitFrame().  commitFrame() marks the surface dirty; the actual
 *      wl_surface_attach + commit happens in the wl_surface.frame callback
 *      (vsync-aligned, coalesced).  First frame bootstraps the callback chain.
 *  - Event-loop integration: single-threaded QSocketNotifier on
 *    wl_display_get_fd(); uses the prepare_read/read_events/dispatch_pending
 *    sequence to avoid the Wayland fd-starvation deadlock; flushes on
 *    QAbstractEventDispatcher::aboutToBlock.
 *  - wl_buffer.release drives the ring back-pressure (pickFree() discipline
 *    from test_weston / D9b).
 *
 * NOT in this file (deferred to Milestone I):
 *  - wl_seat / wl_pointer / wl_touch / wl_keyboard input bridge.
 *
 * Thread safety: all methods must be called from the Qt GUI thread.
 */
#pragma once

#include <QObject>
#include <QRect>
#include <QSocketNotifier>
#include <functional>
#include <atomic>

// Forward-declare libwayland types to avoid pulling C headers into every TU
// that includes this header.
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_buffer;
struct wl_callback;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwp_linux_dmabuf_v1;
struct zwp_linux_buffer_params_v1;

// libetna2d / etnaviv forward declarations
struct etna2d_device;
struct etna2d_surface;
struct etna_bo;

QT_BEGIN_NAMESPACE

/* Number of ARGB ring buffers.  3 gives the scheduler room: one on screen,
 * one committed (in-flight), one being painted by the blitter. */
static constexpr int WL_RING_SIZE = 3;

/* -----------------------------------------------------------------------
 * ArgbBo — one slot in the ARGB ring
 * ----------------------------------------------------------------------- */
struct ArgbBo {
    etna2d_surface  *surf     = nullptr; // GC520 render target (blitter writes here)
    int              dmabuf_fd = -1;     // dmabuf fd (owned; closed on destroy)
    wl_buffer       *wl_buf   = nullptr; // persistent wl_buffer (created once)
    std::atomic<bool> in_use  { false }; // true while Weston holds a reference
};

/* -----------------------------------------------------------------------
 * QEtnaviv2dWaylandTransport
 * ----------------------------------------------------------------------- */
class QEtnaviv2dWaylandTransport : public QObject
{
    Q_OBJECT

public:
    explicit QEtnaviv2dWaylandTransport(etna2d_device *etnaDev,
                                        int            cardFd,
                                        QObject       *parent = nullptr);
    ~QEtnaviv2dWaylandTransport() override;

    /* Phase 1 initialisation: connect, bind globals, create xdg_toplevel.
     * Pumps the display until the xdg_surface configure round-trip completes
     * so the caller can read outputGeometry() immediately after. */
    bool connect();

    /* Output geometry delivered by the xdg_surface configure callback. */
    QRect outputGeometry() const { return m_outputGeometry; }

    /* Allocate the ARGB ring using the output geometry.
     * Must be called after connect() (needs m_outputGeometry).
     * Returns false if BO allocation or dmabuf export fails. */
    bool allocateRing();

    /* Called from BackingStore::flush() after the blitter has rendered into
     * the current free ring BO.  Marks the surface dirty; the frame callback
     * fires the actual attach+commit.  On the very first call (no pending
     * callback) the commit is issued immediately to bootstrap the chain.
     *
     * bo must be the surface returned by currentBo() — this is how the
     * transport knows which slot to attach. */
    void commitFrame(etna2d_surface *bo);

    /* Returns the ring BO the blitter should render into for the next frame.
     * Returns nullptr if all slots are in_use (back-pressure). */
    etna2d_surface *currentBo();

    /* Disconnect and free all Wayland and BO resources. */
    void disconnect();

    /* Is the transport connected and operational? */
    bool isConnected() const { return m_connected; }

private slots:
    void onWaylandReadable();
    void onAboutToBlock();

private:
    /* Wayland registry listener callbacks (static trampolines) */
    static void registryGlobal(void *data, wl_registry *reg,
                               uint32_t name, const char *iface,
                               uint32_t version);
    static void registryGlobalRemove(void *data, wl_registry *reg,
                                     uint32_t name);

    /* xdg_wm_base ping */
    static void xdgWmBasePing(void *data, xdg_wm_base *base, uint32_t serial);

    /* xdg_surface configure */
    static void xdgSurfaceConfigure(void *data, xdg_surface *surf,
                                    uint32_t serial);

    /* xdg_toplevel configure + close */
    static void xdgToplevelConfigure(void *data, xdg_toplevel *tl,
                                     int32_t w, int32_t h,
                                     struct wl_array *states);
    static void xdgToplevelClose(void *data, xdg_toplevel *tl);

    /* zwp_linux_dmabuf_v1 format/modifier (ignored — we use LINEAR) */
    static void dmabufFormat(void *data, zwp_linux_dmabuf_v1 *dmabuf,
                             uint32_t format);
    static void dmabufModifier(void *data, zwp_linux_dmabuf_v1 *dmabuf,
                               uint32_t format,
                               uint32_t modifier_hi, uint32_t modifier_lo);

    /* zwp_linux_buffer_params_v1 created callback */
    static void bufParamsCreated(void *data,
                                 zwp_linux_buffer_params_v1 *params,
                                 wl_buffer *buf);
    static void bufParamsFailed(void *data,
                                zwp_linux_buffer_params_v1 *params);

    /* wl_buffer release callback */
    static void bufferRelease(void *data, wl_buffer *buf);

    /* wl_surface frame callback */
    static void frameCallback(void *data, wl_callback *cb, uint32_t time);

    /* Helpers */
    void armNotifier();           // create QSocketNotifier lazily on first commitFrame()
    void doCommit(int slot);      // attach buf[slot], damage, commit, arm next frame cb
    void armFrameCallback();          // request wl_surface.frame
    int  pickFree() const;            // return first non-in_use slot, or -1

    /* ---- libwayland objects ---- */
    wl_display          *m_display    = nullptr;
    wl_registry         *m_registry   = nullptr;
    wl_compositor       *m_compositor = nullptr;
    wl_surface          *m_surface    = nullptr;
    xdg_wm_base         *m_xdgWmBase  = nullptr;
    xdg_surface         *m_xdgSurface = nullptr;
    xdg_toplevel        *m_xdgToplevel = nullptr;
    zwp_linux_dmabuf_v1 *m_dmabuf     = nullptr;
    wl_callback         *m_frameCallback = nullptr;

    /* ---- BO ring ---- */
    ArgbBo   m_ring[WL_RING_SIZE];
    int      m_currentSlot = 0;   // slot last handed to the blitter

    /* ---- State ---- */
    etna2d_device *m_etnadev   = nullptr;
    int            m_cardFd    = -1;  // DRM card fd for etna2d_surface allocation
    QRect          m_outputGeometry;
    bool           m_connected         = false;
    bool           m_configured        = false; // xdg_surface configure received
    bool           m_dirty             = false; // commitFrame() called, waiting for frame cb
    int            m_pendingCommitSlot = -1;    // slot to attach on next frame cb

    QSocketNotifier *m_notifier = nullptr;
};

QT_END_NAMESPACE
