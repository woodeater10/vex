/* vex – minimal BSP/dwindle WM  (Xinerama multi-monitor edition)
 *
 * Fixes vs. v1:
 *  • Xinerama: every physical monitor gets its own independent BSP tree.
 *  • Z-order: XRaiseWindow on every focus change / click / drag start.
 *  • Ghost-scroll: GrabModeSync passive grab + XAllowEvents(AsyncPointer)
 *    when dragging, ReplayPointer when only focusing — click never leaks.
 *  • Smooth drag: pointer-offset anchor (no delta drift); motion events
 *    are coalesced with XCheckTypedEvent so the window tracks the cursor
 *    1-to-1 even under load.
 *  • Cross-monitor float: dragging a float window to another screen on
 *    ButtonRelease migrates its tree node without recreating it.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/Xinerama.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOD      Mod4Mask
#define MAXBINDS 32
#define MAXMONS   8

/* ── types ───────────────────────────────────────────────────────────── */
typedef struct Node Node;
struct Node {
    int   leaf, floating, horiz;
    float ratio;
    Node *a, *b, *par;
    Window win;
    int x, y, w, h;
};

typedef struct { unsigned int mod; KeyCode code; char cmd[256]; } Bind;

typedef struct { int x, y, w, h; Node *tree, *focused; } Mon;

/* ── globals ─────────────────────────────────────────────────────────── */
static int           gap = 8, bw = 2;
static unsigned long cfocus = 0x5588ff, cnorm = 0x333333;
static char          term[128] = "st";
static Display      *dpy;
static Window        root;
static int           sw, sh;
static Mon           mons[MAXMONS];
static int           nmons = 1, curmon;
static Bind          binds[MAXBINDS];
static int           nbinds;
/* drag state */
static int           drag_mode;          /* 1=move 2=resize, 0=idle    */
static int           drag_mon;           /* owning monitor at drag start */
static int           drag_ox, drag_oy;   /* pointer root-coords at press */
static int           drag_wx, drag_wy;   /* window position at press     */
static int           drag_ww, drag_wh;   /* window size at press         */
static Node         *drag_node;

/* ── tiny helpers ────────────────────────────────────────────────────── */
static int   xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static unsigned long hcol(const char *s)
    { return strtoul(*s == '#' ? s+1 : s, NULL, 16); }

static int monforpt(int x, int y)
{
    for (int i = 0; i < nmons; i++)
        if (x >= mons[i].x && x < mons[i].x+mons[i].w &&
            y >= mons[i].y && y < mons[i].y+mons[i].h) return i;
    return 0;
}

/* ── node helpers ────────────────────────────────────────────────────── */
static Node *mkleaf(Window w)
{
    Node *n = calloc(1, sizeof *n);
    n->leaf = 1; n->ratio = 0.5f; n->win = w;
    return n;
}

static Node *findleaf(Node *n, Window w)
{
    if (!n) return NULL;
    if (n->leaf) return n->win == w ? n : NULL;
    Node *r = findleaf(n->a, w);
    return r ? r : findleaf(n->b, w);
}

/* Which monitor's tree contains window w? */
static int monforwin(Window w)
{
    for (int i = 0; i < nmons; i++)
        if (findleaf(mons[i].tree, w)) return i;
    return curmon;
}

static Node *firstleaf(Node *n)
    { while (n && !n->leaf) n = n->a; return n; }

static Node *nextleaf(int mi, Node *cur)
{
    Node *tree = mons[mi].tree;
    if (!cur || !tree) return firstleaf(tree);
    Node *n = cur;
    while (n->par) {
        if (n->par->a == n) {
            Node *r = firstleaf(n->par->b);
            if (r) return r;
        }
        n = n->par;
    }
    return firstleaf(tree);
}

/* ── borders & layout ────────────────────────────────────────────────── */
static void drawborder(Node *n, int focused)
{
    XSetWindowBorderWidth(dpy, n->win, bw);
    XSetWindowBorder(dpy, n->win, focused ? cfocus : cnorm);
}

static void raise_floating(Node *n)
{
    if (!n) return;
    if (n->leaf) { if (n->floating) XRaiseWindow(dpy, n->win); return; }
    raise_floating(n->a);
    raise_floating(n->b);
}

static void tilenode(Node *n, int x, int y, int w, int h, Node *foc)
{
    if (!n) return;
    n->x = x; n->y = y; n->w = w; n->h = h;
    if (n->leaf) {
        drawborder(n, n == foc);
        if (!n->floating) {
            int tw = w - 2*gap - 2*bw;
            int th = h - 2*gap - 2*bw;
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
            XMoveResizeWindow(dpy, n->win, x+gap, y+gap, tw, th);
        }
        return;
    }
    if (n->horiz) {
        int wa = (int)(w * n->ratio);
        tilenode(n->a, x,    y, wa,   h, foc);
        tilenode(n->b, x+wa, y, w-wa, h, foc);
    } else {
        int ha = (int)(h * n->ratio);
        tilenode(n->a, x, y,    w, ha,   foc);
        tilenode(n->b, x, y+ha, w, h-ha, foc);
    }
}

static void retile(void)
{
    for (int i = 0; i < nmons; i++) {
        Mon *m = &mons[i];
        tilenode(m->tree, m->x, m->y, m->w, m->h, m->focused);
        raise_floating(m->tree);
    }
    XSync(dpy, 0);
}

/* ── focus / insert / detach ─────────────────────────────────────────── */
static void setfocus(int mi, Node *n)
{
    if (!n || !n->leaf) return;
    Node *prev = mons[mi].focused;
    mons[mi].focused = n;
    curmon = mi;
    if (prev && prev != n) drawborder(prev, 0);
    XSetInputFocus(dpy, n->win, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, n->win);
    drawborder(n, 1);
    raise_floating(mons[mi].tree);
}

/* Attach an already-allocated leaf node into monitor mi's tree. */
static void attach(int mi, Node *leaf)
{
    Mon *m = &mons[mi];
    leaf->par = NULL;
    if (!m->tree) { m->tree = leaf; m->focused = leaf; return; }
    Node *t = m->focused && m->focused->leaf ? m->focused : firstleaf(m->tree);
    int horiz = t->w > 0 ? (t->w >= t->h) : (m->w >= m->h);
    Node *sp  = calloc(1, sizeof *sp);
    sp->ratio = 0.5f; sp->horiz = horiz; sp->par = t->par;
    sp->a = t; sp->b = leaf;
    if (!t->par)             m->tree   = sp;
    else if (t->par->a == t) t->par->a = sp;
    else                     t->par->b = sp;
    t->par = sp; leaf->par = sp;
    m->focused = leaf;
}

/* Create a new leaf for window w and attach it. */
static void insert(int mi, Window w) { attach(mi, mkleaf(w)); }

/* Remove leaf n from monitor mi. Frees the split parent; does NOT free n. */
static void detach(int mi, Node *n)
{
    Mon *m = &mons[mi];
    if (!n->par) { m->tree = NULL; m->focused = NULL; return; }
    Node *p = n->par, *sib = p->a == n ? p->b : p->a;
    sib->par = p->par;
    if (!p->par)             m->tree   = sib;
    else if (p->par->a == p) p->par->a = sib;
    else                     p->par->b = sib;
    if (m->focused == n) m->focused = firstleaf(sib);
    free(p);
    n->par = NULL;
}

static void removewin(Window w)
{
    int mi   = monforwin(w);
    Node *n  = findleaf(mons[mi].tree, w);
    if (!n) return;
    if (drag_node == n) { drag_node = NULL; drag_mode = 0; }
    detach(mi, n);
    free(n);
}

static void spawn(const char *cmd)
{
    if (fork() == 0) { setsid(); execlp("sh","sh","-c",cmd,NULL); _exit(0); }
}

/* ── config ──────────────────────────────────────────────────────────── */
static void loadcfg(void)
{
    const char *paths[] = { "/etc/vex/config.conf", "./config.conf", NULL };
    FILE *f = NULL;
    for (int i = 0; paths[i] && !f; i++) f = fopen(paths[i], "r");
    if (!f) return;

    char line[512], k[64], v[448], rawbinds[MAXBINDS][512];
    int  nraw = 0;

    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (sscanf(p, "%63[^ =] = %447[^\n]", k, v) != 2) continue;
        char *e = v + strlen(v) - 1;
        while (e >= v && (*e=='\r'||*e=='\n'||*e==' ')) *e-- = 0;
        if      (!strcmp(k,"gap"))           gap    = atoi(v);
        else if (!strcmp(k,"border"))        bw     = atoi(v);
        else if (!strcmp(k,"border_focus"))  cfocus = hcol(v);
        else if (!strcmp(k,"border_normal")) cnorm  = hcol(v);
        else if (!strcmp(k,"terminal"))      snprintf(term, sizeof(term), "%s", v);
        else if (!strcmp(k,"bind") && nraw < MAXBINDS)
            strncpy(rawbinds[nraw++], v, 511);
    }
    fclose(f);

    for (int i = 0; i < nraw && nbinds < MAXBINDS; i++) {
        char *eq = strrchr(rawbinds[i], '='); if (!eq) continue;
        *eq = 0;
        char *keys = rawbinds[i], *cmd = eq+1;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        char *ke = eq-1;
        while (ke > keys && (*ke==' '||*ke=='\t')) *ke-- = 0;

        unsigned int mask = 0;
        if (strstr(keys,"mod"))   mask |= Mod4Mask;
        if (strstr(keys,"shift")) mask |= ShiftMask;
        if (strstr(keys,"ctrl"))  mask |= ControlMask;
        if (strstr(keys,"alt"))   mask |= Mod1Mask;

        char *kp = strrchr(keys, '+'); kp = kp ? kp+1 : keys;
        while (*kp == ' ') kp++;
        if (!*kp) continue;
        char *ke2 = kp + strlen(kp) - 1;
        while (ke2 > kp && (*ke2==' '||*ke2=='\t')) *ke2-- = 0;

        KeySym sym = XStringToKeysym(kp);
        if (sym == NoSymbol && strlen(kp) == 1) sym = (KeySym)(unsigned char)*kp;
        if (sym == NoSymbol) continue;
        KeyCode code = XKeysymToKeycode(dpy, sym);
        if (!code) continue;

        XGrabKey(dpy, code, mask,           root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, code, mask|LockMask,  root, True, GrabModeAsync, GrabModeAsync);
        binds[nbinds].code = code;
        binds[nbinds].mod  = mask;
        strncpy(binds[nbinds].cmd, cmd, 255);
        nbinds++;
    }
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGCHLD, SIG_IGN);
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    XSetErrorHandler(xerr);

    int scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw   = DisplayWidth(dpy, scr);
    sh   = DisplayHeight(dpy, scr);

    /* ── Xinerama monitor detection ── */
    int xiev, xierr;
    if (XineramaQueryExtension(dpy, &xiev, &xierr) && XineramaIsActive(dpy)) {
        int n; XineramaScreenInfo *xi = XineramaQueryScreens(dpy, &n);
        nmons = n < MAXMONS ? n : MAXMONS;
        for (int i = 0; i < nmons; i++) {
            mons[i].x = xi[i].x_org;  mons[i].y = xi[i].y_org;
            mons[i].w = xi[i].width;  mons[i].h = xi[i].height;
        }
        XFree(xi);
    } else {
        nmons = 1;
        mons[0].x = 0; mons[0].y = 0; mons[0].w = sw; mons[0].h = sh;
    }

    XSelectInput(dpy, root,
        SubstructureNotifyMask|SubstructureRedirectMask|KeyPressMask);

    loadcfg();

#define G(sym, mod) do { \
    KeyCode _c = XKeysymToKeycode(dpy, sym); \
    XGrabKey(dpy,_c,mod,          root,True,GrabModeAsync,GrabModeAsync); \
    XGrabKey(dpy,_c,mod|LockMask, root,True,GrabModeAsync,GrabModeAsync); \
} while(0)
    G(XK_Return, MOD);
    G(XK_q,      MOD);
    G(XK_q,      MOD|ShiftMask);
    G(XK_Tab,    MOD);
    G(XK_f,      MOD);
    G(XK_h,      MOD);
    G(XK_l,      MOD);
    G(XK_v,      MOD);
#undef G

    /*
     * GrabModeSync (pointer_mode) freezes the pointer on ButtonPress until
     * we call XAllowEvents.  owner_events=False means the event is always
     * reported to root, never to the application under the cursor — this
     * is the key to eliminating ghost scroll / input leaks.
     */
    XGrabButton(dpy, Button1, MOD, root, False,
        ButtonPressMask|ButtonReleaseMask,
        GrabModeSync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, MOD, root, False,
        ButtonPressMask|ButtonReleaseMask,
        GrabModeSync, GrabModeAsync, None, None);

    XEvent ev;
    for (;;) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {

        /* ── window mapping ── */
        case MapRequest:
            XSelectInput(dpy, ev.xmaprequest.window, EnterWindowMask);
            XMapWindow(dpy, ev.xmaprequest.window);
            {
                /* Place window on the monitor under the pointer */
                Window dw; int rx,ry,wx,wy; unsigned msk;
                XQueryPointer(dpy,root,&dw,&dw,&rx,&ry,&wx,&wy,&msk);
                int mi = monforpt(rx, ry);
                insert(mi, ev.xmaprequest.window);
                retile();
                setfocus(mi, mons[mi].focused);
            }
            break;

        case DestroyNotify:
            removewin(ev.xdestroywindow.window);
            retile();
            if (mons[curmon].focused) setfocus(curmon, mons[curmon].focused);
            break;

        case UnmapNotify:
            if (ev.xunmap.send_event) {
                removewin(ev.xunmap.window);
                retile();
                if (mons[curmon].focused) setfocus(curmon, mons[curmon].focused);
            }
            break;

        case ConfigureRequest: {
            XWindowChanges wc = {
                .x            = ev.xconfigurerequest.x,
                .y            = ev.xconfigurerequest.y,
                .width        = ev.xconfigurerequest.width,
                .height       = ev.xconfigurerequest.height,
                .border_width = bw,
                .sibling      = ev.xconfigurerequest.above,
                .stack_mode   = ev.xconfigurerequest.detail,
            };
            XConfigureWindow(dpy, ev.xconfigurerequest.window,
                ev.xconfigurerequest.value_mask, &wc);
            break;
        }

        case EnterNotify:
            if (ev.xcrossing.mode == NotifyNormal) {
                int mi   = monforwin(ev.xcrossing.window);
                Node *n  = findleaf(mons[mi].tree, ev.xcrossing.window);
                if (n && n != mons[mi].focused) setfocus(mi, n);
            }
            break;

        /* ── mouse: button press ── */
        case ButtonPress: {
            Window clicked = ev.xbutton.subwindow
                ? ev.xbutton.subwindow : ev.xbutton.window;

            /* Background click — nothing to manage */
            if (clicked == root) {
                XAllowEvents(dpy, ReplayPointer, ev.xbutton.time);
                break;
            }

            /* Find the node across all monitors */
            int mi = monforpt(ev.xbutton.x_root, ev.xbutton.y_root);
            Node *n = NULL;
            for (int i = 0; i < nmons && !n; i++)
                if ((n = findleaf(mons[i].tree, clicked))) mi = i;

            if (n && n->floating) {
                /*
                 * We are going to take control of this click for dragging.
                 * AsyncPointer unfreezes the pointer but does NOT replay the
                 * event to the client — the application never sees the press.
                 */
                XAllowEvents(dpy, AsyncPointer, ev.xbutton.time);

                /* Raise and focus */
                if (n != mons[mi].focused) setfocus(mi, n);
                else { XRaiseWindow(dpy, n->win); raise_floating(mons[mi].tree); }

                /* Snapshot geometry for drag math */
                Window dw; unsigned gw, gh, gb, gd;
                XGetGeometry(dpy, n->win, &dw,
                    &drag_wx, &drag_wy, &gw, &gh, &gb, &gd);
                drag_ww = (int)gw; drag_wh = (int)gh;
                /* Anchor: cursor position in root coords at press time */
                drag_ox = ev.xbutton.x_root;
                drag_oy = ev.xbutton.y_root;
                drag_node = n; drag_mon = mi;
                drag_mode = ev.xbutton.button == Button1 ? 1 : 2;

                /* Exclusive pointer grab — no further events reach children */
                XGrabPointer(dpy, root, False,
                    PointerMotionMask|ButtonReleaseMask,
                    GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            } else {
                /*
                 * Pure focus click: replay so the application receives it
                 * normally (important for terminal / browser focus).
                 */
                XAllowEvents(dpy, ReplayPointer, ev.xbutton.time);
                if (n && n != mons[mi].focused) { setfocus(mi, n); retile(); }
                else if (n) XRaiseWindow(dpy, n->win);
            }
            break;
        }

        /* ── mouse: button release ── */
        case ButtonRelease:
            if (drag_mode) {
                XUngrabPointer(dpy, CurrentTime);
                /*
                 * If the floating window was dragged onto a different monitor,
                 * migrate its tree node.  We reuse the existing Node* (detach +
                 * attach) so no allocation/free is needed.
                 */
                if (drag_node) {
                    Window dw; int rx,ry,wx2,wy2; unsigned msk;
                    XQueryPointer(dpy,root,&dw,&dw,&rx,&ry,&wx2,&wy2,&msk);
                    int newmon = monforpt(rx, ry);
                    if (newmon != drag_mon) {
                        int was_floating = drag_node->floating;
                        detach(drag_mon, drag_node);
                        attach(newmon, drag_node);
                        drag_node->floating = was_floating;
                        curmon = newmon;
                        retile();
                    }
                }
            }
            drag_mode = 0; drag_node = NULL;
            break;

        /* ── mouse: motion ── */
        case MotionNotify:
            if (!drag_mode || !drag_node) break;
            /*
             * Coalesce pending motion events: keep only the most recent
             * position so the window tracks the cursor 1-to-1 under load.
             */
            { XEvent _m; while (XCheckTypedEvent(dpy, MotionNotify, &_m)) ev = _m; }
            {
                int dx = ev.xmotion.x_root - drag_ox;
                int dy = ev.xmotion.y_root - drag_oy;
                if (drag_mode == 1) {          /* move */
                    int nx = drag_wx + dx, ny = drag_wy + dy;
                    /* Clamp within the full virtual desktop so drags across
                       monitors are unrestricted; per-monitor edges are soft. */
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx + drag_ww + 2*bw > sw) nx = sw - drag_ww - 2*bw;
                    if (ny + drag_wh + 2*bw > sh) ny = sh - drag_wh - 2*bw;
                    XMoveWindow(dpy, drag_node->win, nx, ny);
                } else {                       /* resize */
                    int nw = drag_ww + dx, nh = drag_wh + dy;
                    if (nw < 50) nw = 50;
                    if (nh < 50) nh = 50;
                    if (drag_wx + nw + 2*bw > sw) nw = sw - drag_wx - 2*bw;
                    if (drag_wy + nh + 2*bw > sh) nh = sh - drag_wy - 2*bw;
                    XResizeWindow(dpy, drag_node->win, nw, nh);
                }
            }
            break;

        /* ── keyboard ── */
        case KeyPress: {
            unsigned int s = ev.xkey.state
                & (ShiftMask|ControlMask|Mod1Mask|Mod4Mask);
            KeySym sym = XLookupKeysym(&ev.xkey, 0);

            int handled = 0;
            for (int i = 0; i < nbinds; i++) {
                if (ev.xkey.keycode == binds[i].code && s == binds[i].mod) {
                    spawn(binds[i].cmd); handled = 1; break;
                }
            }
            if (handled) break;

            Mon *m = &mons[curmon];
            if (sym == XK_Return && s == MOD) {
                spawn(term);
            } else if (sym == XK_q && s == (MOD|ShiftMask)) {
                XCloseDisplay(dpy); return 0;
            } else if (sym == XK_q && s == MOD && m->focused) {
                XKillClient(dpy, m->focused->win);
            } else if (sym == XK_Tab && s == MOD) {
                Node *n = nextleaf(curmon, m->focused);
                if (n && n != m->focused) setfocus(curmon, n);
            } else if (sym == XK_f && s == MOD && m->focused) {
                m->focused->floating ^= 1;
                if (m->focused->floating) {
                    int fw = m->w/2, fh = m->h/2;
                    int fx = m->x + (m->w - fw)/2;
                    int fy = m->y + (m->h - fh)/2;
                    XMoveResizeWindow(dpy, m->focused->win,
                        fx, fy, fw - 2*bw, fh - 2*bw);
                    XRaiseWindow(dpy, m->focused->win);
                }
                retile();
            } else if (sym == XK_h && s == MOD && m->focused && m->focused->par) {
                m->focused->par->ratio -= 0.05f;
                if (m->focused->par->ratio < 0.1f) m->focused->par->ratio = 0.1f;
                retile();
            } else if (sym == XK_l && s == MOD && m->focused && m->focused->par) {
                m->focused->par->ratio += 0.05f;
                if (m->focused->par->ratio > 0.9f) m->focused->par->ratio = 0.9f;
                retile();
            } else if (sym == XK_v && s == MOD && m->focused && m->focused->par) {
                m->focused->par->horiz ^= 1;
                retile();
            }
            break;
        }

        } /* switch */
    } /* event loop */
}
