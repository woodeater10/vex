#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOD      Mod4Mask
#define MAXBINDS 32

static int gap = 8, bw = 2;
static unsigned long cfocus = 0x5588ff, cnorm = 0x333333;
static char term[128] = "st";

typedef struct Node Node;
struct Node {
    int   leaf, floating, horiz;
    float ratio;
    Node *a, *b, *par;
    Window win;
    int x, y, w, h;
};

typedef struct { unsigned int mod; KeyCode code; char cmd[256]; } Bind;

static Display *dpy;
static Window   root;
static int      sw, sh;
static Node    *tree, *focused;
static Bind     binds[MAXBINDS];
static int      nbinds;

static int   drag_mode;
static int   drag_ox, drag_oy, drag_wx, drag_wy, drag_ww, drag_wh;
static Node *drag_node;

static int          xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static unsigned long hcol(const char *s)              { return strtoul(*s=='#'?s+1:s, NULL, 16); }

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

static Node *firstleaf(Node *n)
{
    while (n && !n->leaf) n = n->a;
    return n;
}

static Node *nextleaf(Node *cur)
{
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

static void border(Node *n)
{
    XSetWindowBorderWidth(dpy, n->win, bw);
    XSetWindowBorder(dpy, n->win, n == focused ? cfocus : cnorm);
}

static void raise_floating(Node *n)
{
    if (!n) return;
    if (n->leaf) { if (n->floating) XRaiseWindow(dpy, n->win); return; }
    raise_floating(n->a);
    raise_floating(n->b);
}

static void tilenode(Node *n, int x, int y, int w, int h)
{
    if (!n) return;
    n->x = x; n->y = y; n->w = w; n->h = h;
    if (n->leaf) {
        border(n);
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
        tilenode(n->a, x,    y, wa,   h);
        tilenode(n->b, x+wa, y, w-wa, h);
    } else {
        int ha = (int)(h * n->ratio);
        tilenode(n->a, x, y,    w, ha);
        tilenode(n->b, x, y+ha, w, h-ha);
    }
}

static void retile(void)
{
    tilenode(tree, 0, 0, sw, sh);
    raise_floating(tree);
    XSync(dpy, 0);
}

static void setfocus(Node *n)
{
    if (!n || !n->leaf) return;
    Node *prev = focused;
    focused = n;
    if (prev && prev != n) border(prev);
    XSetInputFocus(dpy, n->win, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, n->win);
    border(n);
    raise_floating(tree);
}

static void insert(Window w)
{
    Node *leaf = mkleaf(w);
    if (!tree) { tree = leaf; focused = leaf; return; }
    Node *t = focused && focused->leaf ? focused : firstleaf(tree);
    int horiz = t->w > 0 ? (t->w >= t->h) : (sw >= sh);
    Node *sp = calloc(1, sizeof *sp);
    sp->ratio = 0.5f; sp->horiz = horiz;
    sp->a = t; sp->b = leaf; sp->par = t->par;
    if (!t->par)         tree         = sp;
    else if (t->par->a == t) t->par->a = sp;
    else                 t->par->b = sp;
    t->par = sp; leaf->par = sp;
    focused = leaf;
}

static void removewin(Window w)
{
    Node *n = findleaf(tree, w);
    if (!n) return;
    if (drag_node == n) { drag_node = NULL; drag_mode = 0; }
    if (!n->par) { free(n); tree = NULL; focused = NULL; return; }
    Node *p   = n->par;
    Node *sib = p->a == n ? p->b : p->a;
    sib->par = p->par;
    if (!p->par)         tree         = sib;
    else if (p->par->a == p) p->par->a = sib;
    else                 p->par->b = sib;
    if (focused == n) focused = firstleaf(sib);
    free(n); free(p);
}

static void spawn(const char *cmd)
{
    if (fork() == 0) { setsid(); execlp("sh","sh","-c",cmd,NULL); _exit(0); }
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
        char *end = v + strlen(v) - 1;
        while (end >= v && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = 0;
        if      (!strcmp(k,"gap"))           gap    = atoi(v);
        else if (!strcmp(k,"border"))        bw     = atoi(v);
        else if (!strcmp(k,"border_focus"))  cfocus = hcol(v);
        else if (!strcmp(k,"border_normal")) cnorm  = hcol(v);
        else if (!strcmp(k,"terminal"))      strncpy(term, v, sizeof(term)-1);
        else if (!strcmp(k,"bind") && nraw < MAXBINDS)
            strncpy(rawbinds[nraw++], v, 511);
    }
    fclose(f);

    for (int i = 0; i < nraw && nbinds < MAXBINDS; i++) {
        char buf[512];
        strncpy(buf, rawbinds[i], 511);
        char *eq = strrchr(buf, '=');
        if (!eq) continue;
        *eq = 0;
        char *keys = buf, *cmd = eq+1;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        char *ke = eq - 1;
        while (ke > keys && (*ke == ' ' || *ke == '\t')) *ke-- = 0;

        unsigned int mask = 0;
        if (strstr(keys,"mod"))   mask |= Mod4Mask;
        if (strstr(keys,"shift")) mask |= ShiftMask;
        if (strstr(keys,"ctrl"))  mask |= ControlMask;
        if (strstr(keys,"alt"))   mask |= Mod1Mask;

        char *kp = strrchr(keys, '+');
        kp = kp ? kp+1 : keys;
        while (*kp == ' ') kp++;
        if (!*kp) continue;
        char *ke2 = kp + strlen(kp) - 1;
        while (ke2 > kp && (*ke2 == ' ' || *ke2 == '\t')) *ke2-- = 0;

        KeySym sym = XStringToKeysym(kp);
        if (sym == NoSymbol && strlen(kp) == 1)
            sym = (KeySym)(unsigned char)*kp;
        if (sym == NoSymbol) continue;
        KeyCode code = XKeysymToKeycode(dpy, sym);
        if (!code) continue;

        XGrabKey(dpy, code, mask, root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, code, mask|LockMask, root, True, GrabModeAsync, GrabModeAsync);
        binds[nbinds].code = code;
        binds[nbinds].mod  = mask;
        strncpy(binds[nbinds].cmd, cmd, 255);
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

    XSelectInput(dpy, root,
        SubstructureNotifyMask | SubstructureRedirectMask | KeyPressMask);

    loadcfg();

#define G(sym, mod) do { \
    KeyCode _c = XKeysymToKeycode(dpy, sym); \
    XGrabKey(dpy, _c, mod, root, True, GrabModeAsync, GrabModeAsync); \
    XGrabKey(dpy, _c, mod|LockMask, root, True, GrabModeAsync, GrabModeAsync); \
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

    XGrabButton(dpy, Button1, MOD, root, False,
        ButtonPressMask|ButtonReleaseMask,
        GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, Button3, MOD, root, False,
        ButtonPressMask|ButtonReleaseMask,
        GrabModeAsync, GrabModeAsync, None, None);

    XEvent ev;
    for (;;) {
        XNextEvent(dpy, &ev);
        switch (ev.type) {

        case MapRequest:
            XSelectInput(dpy, ev.xmaprequest.window, EnterWindowMask);
            XMapWindow(dpy, ev.xmaprequest.window);
            insert(ev.xmaprequest.window);
            retile();
            setfocus(focused);
            break;

        case DestroyNotify:
            removewin(ev.xdestroywindow.window);
            retile();
            if (focused) setfocus(focused);
            break;

        case UnmapNotify:
            if (ev.xunmap.send_event) {
                removewin(ev.xunmap.window);
                retile();
                if (focused) setfocus(focused);
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
                Node *n = findleaf(tree, ev.xcrossing.window);
                if (n && n != focused) { setfocus(n); border(n); }
            }
            break;

        case ButtonPress: {
            Window clicked = ev.xbutton.subwindow
                ? ev.xbutton.subwindow : ev.xbutton.window;
            if (clicked == root) break;
            Node *n = findleaf(tree, clicked);
            if (n && n != focused) { setfocus(n); retile(); }
            if (n && n->floating) {
                Window   gdummy;
                unsigned gw, gh, gbw, gd;
                XGetGeometry(dpy, n->win, &gdummy,
                    &drag_wx, &drag_wy, &gw, &gh, &gbw, &gd);
                drag_ww = (int)gw; drag_wh = (int)gh;
                drag_ox = ev.xbutton.x_root;
                drag_oy = ev.xbutton.y_root;
                drag_node = n;
                drag_mode = ev.xbutton.button == Button1 ? 1 : 2;
                XGrabPointer(dpy, root, False,
                    PointerMotionMask|ButtonReleaseMask,
                    GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            }
            break;
        }

        case ButtonRelease:
            if (drag_mode) XUngrabPointer(dpy, CurrentTime);
            drag_mode = 0; drag_node = NULL;
            break;

        case MotionNotify:
            if (!drag_mode || !drag_node) break;
            {
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
                    if (nw < 50) nw = 50;
                    if (nh < 50) nh = 50;
                    if (drag_wx + nw + 2*bw > sw) nw = sw - drag_wx - 2*bw;
                    if (drag_wy + nh + 2*bw > sh) nh = sh - drag_wy - 2*bw;
                    XResizeWindow(dpy, drag_node->win, nw, nh);
                }
            }
            break;

        case KeyPress: {
            unsigned int s = ev.xkey.state
                & (ShiftMask|ControlMask|Mod1Mask|Mod4Mask);
            KeySym sym = XLookupKeysym(&ev.xkey, 0);

            int handled = 0;
            for (int i = 0; i < nbinds; i++) {
                if (ev.xkey.keycode == binds[i].code && s == binds[i].mod) {
                    spawn(binds[i].cmd);
                    handled = 1; break;
                }
            }
            if (handled) break;

            if (sym == XK_Return && s == MOD) {
                spawn(term);
            } else if (sym == XK_q && s == (MOD|ShiftMask)) {
                XCloseDisplay(dpy);
                return 0;
            } else if (sym == XK_q && s == MOD && focused) {
                XKillClient(dpy, focused->win);
            } else if (sym == XK_Tab && s == MOD) {
                Node *n = nextleaf(focused);
                if (n && n != focused) { setfocus(n); border(n); }
            } else if (sym == XK_f && s == MOD && focused) {
                focused->floating ^= 1;
                if (focused->floating) {
                    int fw = sw/2, fh = sh/2;
                    int fx = (sw-fw)/2, fy = (sh-fh)/2;
                    XMoveResizeWindow(dpy, focused->win,
                        fx, fy, fw-2*bw, fh-2*bw);
                    XRaiseWindow(dpy, focused->win);
                }
                retile();
            } else if (sym == XK_h && s == MOD && focused && focused->par) {
                focused->par->ratio -= 0.05f;
                if (focused->par->ratio < 0.1f) focused->par->ratio = 0.1f;
                retile();
            } else if (sym == XK_l && s == MOD && focused && focused->par) {
                focused->par->ratio += 0.05f;
                if (focused->par->ratio > 0.9f) focused->par->ratio = 0.9f;
                retile();
            } else if (sym == XK_v && s == MOD && focused && focused->par) {
                focused->par->horiz ^= 1;
                retile();
            }
            break;
        }

        }
    }
}
