/* Vex the tiling wm made by woodeater */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/Xinerama.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOD      Mod4Mask
#define MAXBINDS 64
#define MAXMONS   8
#define MINSIZE  50

typedef struct Node Node;
struct Node {
    int    leaf, floating, horiz;
    float  ratio;
    Node  *a, *b, *par;
    Window win;
    int    x, y, w, h;
};

typedef struct { unsigned int mod; KeyCode code; char cmd[256]; } Bind;
typedef struct { int x, y, w, h; Node *tree, *focus; } Mon;

static Display      *dpy;
static Window        root;
static int           sw, sh;
static int           gap = 8, bw = 2;
static unsigned long cfocus = 0x5588ff, cnorm = 0x333333;
static char          term[128] = "st";
static Mon           mons[MAXMONS];
static int           nmons = 1, curmon = 0;
static Bind          binds[MAXBINDS];
static int           nbinds;
static int           drag_mode, drag_mon;
static int           drag_ox, drag_oy, drag_wx, drag_wy, drag_ww, drag_wh;
static Node         *drag_node;

static int           xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static unsigned long hcol(const char *s) { return strtoul(*s == '#' ? s+1 : s, NULL, 16); }

static int monforpt(int x, int y)
{
    for (int i = 0; i < nmons; i++)
        if (x >= mons[i].x && x < mons[i].x + mons[i].w &&
            y >= mons[i].y && y < mons[i].y + mons[i].h) return i;
    return 0;
}

static int monforwin(Window w)
{
    for (int i = 0; i < nmons; i++) {
        Node *n = mons[i].tree;
        if (!n) continue;
        Node *stack[512]; int top = 0;
        stack[top++] = n;
        while (top) {
            Node *c = stack[--top];
            if (c->leaf) { if (c->win == w) return i; }
            else { stack[top++] = c->a; stack[top++] = c->b; }
        }
    }
    return curmon;
}

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

static Node *firstleaf(Node *n) { while (n && !n->leaf) n = n->a; return n; }

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

static void drawborder(Node *n, int focused)
{
    XSetWindowBorderWidth(dpy, n->win, bw);
    XSetWindowBorder(dpy, n->win, focused ? cfocus : cnorm);
}

static void raise_floats(Node *n)
{
    if (!n) return;
    if (n->leaf) { if (n->floating) XRaiseWindow(dpy, n->win); return; }
    raise_floats(n->a);
    raise_floats(n->b);
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
            XMoveResizeWindow(dpy, n->win, x+gap, y+gap,
                tw < 1 ? 1 : tw, th < 1 ? 1 : th);
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
        tilenode(m->tree, m->x, m->y, m->w, m->h, m->focus);
        raise_floats(m->tree);
    }
    XSync(dpy, 0);
}

static void setfocus(int mi, Node *n)
{
    if (!n || !n->leaf) return;
    Node *prev = mons[mi].focus;
    mons[mi].focus = n;
    curmon = mi;
    if (prev && prev != n) drawborder(prev, 0);
    drawborder(n, 1);
    XRaiseWindow(dpy, n->win);
    raise_floats(mons[mi].tree);
    XSetInputFocus(dpy, n->win, RevertToPointerRoot, CurrentTime);
}

static void attach(int mi, Node *leaf)
{
    Mon *m = &mons[mi];
    leaf->par = NULL;
    if (!m->tree) { m->tree = leaf; m->focus = leaf; return; }
    Node *t = m->focus && m->focus->leaf ? m->focus : firstleaf(m->tree);
    int horiz = t->w > 0 ? (t->w >= t->h) : (m->w >= m->h);
    Node *sp = calloc(1, sizeof *sp);
    sp->ratio = 0.5f; sp->horiz = horiz; sp->par = t->par;
    sp->a = t; sp->b = leaf;
    if (!t->par)             m->tree   = sp;
    else if (t->par->a == t) t->par->a = sp;
    else                     t->par->b = sp;
    t->par = sp; leaf->par = sp;
    m->focus = leaf;
}

static void detach(int mi, Node *n)
{
    Mon *m = &mons[mi];
    if (!n->par) { m->tree = NULL; m->focus = NULL; return; }
    Node *p = n->par, *sib = p->a == n ? p->b : p->a;
    sib->par = p->par;
    if (!p->par)             m->tree   = sib;
    else if (p->par->a == p) p->par->a = sib;
    else                     p->par->b = sib;
    if (m->focus == n) m->focus = firstleaf(sib);
    free(p);
    n->par = NULL;
}

static void removewin(Window w)
{
    int mi = monforwin(w);
    Node *n = findleaf(mons[mi].tree, w);
    if (!n) return;
    if (drag_node == n) { drag_node = NULL; drag_mode = 0; }
    detach(mi, n);
    free(n);
}

static void spawn(const char *cmd)
{
    if (fork() == 0) { setsid(); execlp("sh", "sh", "-c", cmd, NULL); _exit(0); }
}

static void loadcfg(void)
{
    const char *paths[] = { "/etc/vex/config.conf", "./config.conf", NULL };
    FILE *f = NULL;
    for (int i = 0; paths[i] && !f; i++) f = fopen(paths[i], "r");
    if (!f) return;

    char line[512], k[64], v[448];
    char rawbinds[MAXBINDS][512];
    int  nraw = 0;

    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        if (sscanf(p, "%63[^ =] = %447[^\n]", k, v) != 2) continue;
        char *e = v + strlen(v) - 1;
        while (e >= v && (*e == '\r' || *e == '\n' || *e == ' ')) *e-- = 0;
        if      (!strcmp(k, "gap"))           gap    = atoi(v);
        else if (!strcmp(k, "border"))        bw     = atoi(v);
        else if (!strcmp(k, "border_focus"))  cfocus = hcol(v);
        else if (!strcmp(k, "border_normal")) cnorm  = hcol(v);
        else if (!strcmp(k, "terminal"))      snprintf(term, sizeof term, "%s", v);
        else if (!strcmp(k, "bind") && nraw < MAXBINDS)
            snprintf(rawbinds[nraw++], 512, "%s", v);
    }
    fclose(f);

    for (int i = 0; i < nraw && nbinds < MAXBINDS; i++) {
        char *eq = strrchr(rawbinds[i], '=');
        if (!eq) continue;
        *eq = 0;
        char *keys = rawbinds[i], *cmd = eq + 1;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        char *ke = eq - 1;
        while (ke > keys && (*ke == ' ' || *ke == '\t')) *ke-- = 0;

        unsigned int mask = 0;
        if (strstr(keys, "mod"))   mask |= Mod4Mask;
        if (strstr(keys, "shift")) mask |= ShiftMask;
        if (strstr(keys, "ctrl"))  mask |= ControlMask;
        if (strstr(keys, "alt"))   mask |= Mod1Mask;

        char *kp = strrchr(keys, '+');
        kp = kp ? kp + 1 : keys;
        while (*kp == ' ') kp++;
        if (!*kp) continue;
        char *ke2 = kp + strlen(kp) - 1;
        while (ke2 > kp && (*ke2 == ' ' || *ke2 == '\t')) *ke2-- = 0;

        KeySym sym = XStringToKeysym(kp);
        if (sym == NoSymbol && strlen(kp) == 1) sym = (KeySym)(unsigned char)*kp;
        if (sym == NoSymbol) continue;
        KeyCode code = XKeysymToKeycode(dpy, sym);
        if (!code) continue;

        XGrabKey(dpy, code, mask,          root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, code, mask|LockMask, root, True, GrabModeAsync, GrabModeAsync);
        binds[nbinds].code = code;
        binds[nbinds].mod  = mask;
        snprintf(binds[nbinds].cmd, sizeof binds[nbinds].cmd, "%s", cmd);
        nbinds++;
    }
}

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

    int xiev, xierr;
    if (XineramaQueryExtension(dpy, &xiev, &xierr) && XineramaIsActive(dpy)) {
        int n;
        XineramaScreenInfo *xi = XineramaQueryScreens(dpy, &n);
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
        SubstructureNotifyMask | SubstructureRedirectMask | KeyPressMask);

    loadcfg();

#define GRAB(sym, mod) do { \
    KeyCode _c = XKeysymToKeycode(dpy, sym); \
    XGrabKey(dpy,_c,(mod),         root,True,GrabModeAsync,GrabModeAsync); \
    XGrabKey(dpy,_c,(mod)|LockMask,root,True,GrabModeAsync,GrabModeAsync); \
} while(0)
    GRAB(XK_Return, MOD);
    GRAB(XK_q,      MOD);
    GRAB(XK_q,      MOD|ShiftMask);
    GRAB(XK_Tab,    MOD);
    GRAB(XK_f,      MOD);
    GRAB(XK_h,      MOD);
    GRAB(XK_l,      MOD);
    GRAB(XK_v,      MOD);
#undef GRAB

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

        case MapRequest: {
            XSelectInput(dpy, ev.xmaprequest.window, EnterWindowMask);
            XMapWindow(dpy, ev.xmaprequest.window);
            Window dw; int rx, ry, wx, wy; unsigned msk;
            XQueryPointer(dpy, root, &dw, &dw, &rx, &ry, &wx, &wy, &msk);
            int mi = monforpt(rx, ry);
            attach(mi, mkleaf(ev.xmaprequest.window));
            retile();
            setfocus(mi, mons[mi].focus);
            break;
        }

        case DestroyNotify:
            removewin(ev.xdestroywindow.window);
            retile();
            if (mons[curmon].focus) setfocus(curmon, mons[curmon].focus);
            break;

        case UnmapNotify:
            if (ev.xunmap.send_event) {
                removewin(ev.xunmap.window);
                retile();
                if (mons[curmon].focus) setfocus(curmon, mons[curmon].focus);
            }
            break;

        case ConfigureRequest: {
            XWindowChanges wc = {
                .x = ev.xconfigurerequest.x, .y = ev.xconfigurerequest.y,
                .width = ev.xconfigurerequest.width,
                .height = ev.xconfigurerequest.height,
                .border_width = bw,
                .sibling = ev.xconfigurerequest.above,
                .stack_mode = ev.xconfigurerequest.detail,
            };
            XConfigureWindow(dpy, ev.xconfigurerequest.window,
                ev.xconfigurerequest.value_mask, &wc);
            break;
        }

        case EnterNotify:
            if (ev.xcrossing.mode == NotifyNormal) {
                int mi  = monforwin(ev.xcrossing.window);
                Node *n = findleaf(mons[mi].tree, ev.xcrossing.window);
                if (n && n != mons[mi].focus) setfocus(mi, n);
            }
            break;

        case ButtonPress: {
            Window clicked = ev.xbutton.subwindow
                ? ev.xbutton.subwindow : ev.xbutton.window;

            if (clicked == root) {
                XAllowEvents(dpy, ReplayPointer, ev.xbutton.time);
                break;
            }

            int mi = monforpt(ev.xbutton.x_root, ev.xbutton.y_root);
            Node *n = NULL;
            for (int i = 0; i < nmons && !n; i++)
                if ((n = findleaf(mons[i].tree, clicked))) mi = i;

            if (n && n->floating) {
                XAllowEvents(dpy, AsyncPointer, ev.xbutton.time);
                setfocus(mi, n);

                Window dw; unsigned gw, gh, gb, gd;
                XGetGeometry(dpy, n->win, &dw,
                    &drag_wx, &drag_wy, &gw, &gh, &gb, &gd);
                drag_ww = (int)gw; drag_wh = (int)gh;
                drag_ox = ev.xbutton.x_root;
                drag_oy = ev.xbutton.y_root;
                drag_node = n; drag_mon = mi;
                drag_mode = ev.xbutton.button == Button1 ? 1 : 2;

                XGrabPointer(dpy, root, False,
                    PointerMotionMask|ButtonReleaseMask,
                    GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            } else {
                XAllowEvents(dpy, ReplayPointer, ev.xbutton.time);
                if (n) {
                    setfocus(mi, n);
                    retile();
                }
            }
            break;
        }

        case ButtonRelease:
            if (drag_mode) {
                XUngrabPointer(dpy, CurrentTime);
                if (drag_node) {
                    Window dw; int rx, ry, wx2, wy2; unsigned msk;
                    XQueryPointer(dpy, root, &dw, &dw, &rx, &ry, &wx2, &wy2, &msk);
                    int newmon = monforpt(rx, ry);
                    if (newmon != drag_mon) {
                        int fl = drag_node->floating;
                        detach(drag_mon, drag_node);
                        attach(newmon, drag_node);
                        drag_node->floating = fl;
                        curmon = newmon;
                        retile();
                    }
                }
            }
            drag_mode = 0; drag_node = NULL;
            break;

        case MotionNotify: {
            if (!drag_mode || !drag_node) break;
            XEvent tmp;
            while (XCheckTypedEvent(dpy, MotionNotify, &tmp)) ev = tmp;
            int dx = ev.xmotion.x_root - drag_ox;
            int dy = ev.xmotion.y_root - drag_oy;
            if (drag_mode == 1) {
                int nx = drag_wx + dx, ny = drag_wy + dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx + drag_ww + 2*bw > sw) nx = sw - drag_ww - 2*bw;
                if (ny + drag_wh + 2*bw > sh) ny = sh - drag_wh - 2*bw;
                XMoveWindow(dpy, drag_node->win, nx, ny);
            } else {
                int nw = drag_ww + dx, nh = drag_wh + dy;
                if (nw < MINSIZE) nw = MINSIZE;
                if (nh < MINSIZE) nh = MINSIZE;
                if (drag_wx + nw + 2*bw > sw) nw = sw - drag_wx - 2*bw;
                if (drag_wy + nh + 2*bw > sh) nh = sh - drag_wy - 2*bw;
                XResizeWindow(dpy, drag_node->win, nw, nh);
            }
            break;
        }

        case KeyPress: {
            unsigned int s = ev.xkey.state & (ShiftMask|ControlMask|Mod1Mask|Mod4Mask);
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
            } else if (sym == XK_q && s == MOD && m->focus) {
                XKillClient(dpy, m->focus->win);
            } else if (sym == XK_Tab && s == MOD) {
                Node *n = nextleaf(curmon, m->focus);
                if (n && n != m->focus) setfocus(curmon, n);
            } else if (sym == XK_f && s == MOD && m->focus) {
                m->focus->floating ^= 1;
                if (m->focus->floating) {
                    int fw = m->w / 2, fh = m->h / 2;
                    int fx = m->x + (m->w - fw) / 2;
                    int fy = m->y + (m->h - fh) / 2;
                    XMoveResizeWindow(dpy, m->focus->win,
                        fx, fy, fw - 2*bw, fh - 2*bw);
                }
                retile();
                setfocus(curmon, m->focus);
            } else if (sym == XK_h && s == MOD && m->focus && m->focus->par) {
                float *r = &m->focus->par->ratio;
                *r -= 0.05f; if (*r < 0.1f) *r = 0.1f;
                retile();
            } else if (sym == XK_l && s == MOD && m->focus && m->focus->par) {
                float *r = &m->focus->par->ratio;
                *r += 0.05f; if (*r > 0.9f) *r = 0.9f;
                retile();
            } else if (sym == XK_v && s == MOD && m->focus && m->focus->par) {
                m->focus->par->horiz ^= 1;
                retile();
            }
            break;
        }

        }
    }
}
