/* wm.c */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Node Node;
struct Node {
    int leaf, horiz;
    float ratio;
    Node *a, *b, *par;
    Window win;
    int x, y, w, h;
};

typedef struct {
    unsigned int mod;
    KeyCode code;
    char cmd[256];
} Bind;

static Display      *dpy;
static Window        root;
static int           sw, sh;
static Node         *tree, *focused;
static int           gap = 8, bw = 2;
static unsigned long cfocus = 0x5588ff, cnorm = 0x222222;
static char          term[128] = "st";
static Bind          binds[32];
static int           nbinds;
static char          rawbinds[32][512];
static int           nrawbinds;

static int xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static Node *mkleaf(Window w)
{
    Node *n = calloc(1, sizeof *n);
    n->leaf = 1; n->ratio = 0.5f; n->win = w;
    return n;
}

static Node *mksplit(Node *a, Node *b, int horiz)
{
    Node *n = calloc(1, sizeof *n);
    n->ratio = 0.5f; n->horiz = horiz;
    n->a = a; n->b = b;
    a->par = n; b->par = n;
    return n;
}

static Node *findwin(Node *n, Window w)
{
    if (!n) return NULL;
    if (n->leaf) return n->win == w ? n : NULL;
    Node *r = findwin(n->a, w);
    return r ? r : findwin(n->b, w);
}

static Node *firstleaf(Node *n)
{
    while (n && !n->leaf) n = n->a;
    return n;
}

static Node *nextleaf(Node *cur)
{
    if (!cur) return firstleaf(tree);
    Node *n = cur;
    while (n->par) {
        if (n->par->a == n) return firstleaf(n->par->b);
        n = n->par;
    }
    return firstleaf(tree);
}

static void tilenode(Node *n, int x, int y, int w, int h)
{
    if (!n) return;
    n->x = x; n->y = y; n->w = w; n->h = h;
    if (n->leaf) {
        int ww = w - 2*gap - 2*bw;
        int wh = h - 2*gap - 2*bw;
        XMoveResizeWindow(dpy, n->win, x+gap, y+gap,
            ww < 1 ? 1 : ww, wh < 1 ? 1 : wh);
        XSetWindowBorderWidth(dpy, n->win, bw);
        XSetWindowBorder(dpy, n->win, focused == n ? cfocus : cnorm);
        return;
    }
    if (n->horiz) {
        int wa = (int)(w * n->ratio);
        tilenode(n->a, x, y, wa, h);
        tilenode(n->b, x+wa, y, w-wa, h);
    } else {
        int ha = (int)(h * n->ratio);
        tilenode(n->a, x, y, w, ha);
        tilenode(n->b, x, y+ha, w, h-ha);
    }
}

static void retile(void)
{
    tilenode(tree, 0, 0, sw, sh);
    XSync(dpy, 0);
}

static void setfocus(Node *n)
{
    if (!n || !n->leaf) return;
    focused = n;
    XSetInputFocus(dpy, n->win, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, n->win);
}

static void insert(Window w)
{
    Node *leaf = mkleaf(w);
    if (!tree) { tree = leaf; focused = leaf; return; }
    Node *t = focused ? focused : firstleaf(tree);
    int horiz = t->w >= t->h ? 1 : 0;
    Node *sp = mksplit(t, leaf, horiz);
    sp->par = t->par;
    if (!t->par) tree = sp;
    else if (t->par->a == t) t->par->a = sp;
    else t->par->b = sp;
    t->par = sp;
    focused = leaf;
}

static void removewin(Window w)
{
    Node *n = findwin(tree, w);
    if (!n) return;
    if (!n->par) { free(n); tree = NULL; focused = NULL; return; }
    Node *p = n->par;
    Node *sib = p->a == n ? p->b : p->a;
    sib->par = p->par;
    if (!p->par) tree = sib;
    else if (p->par->a == p) p->par->a = sib;
    else p->par->b = sib;
    Node *nf = firstleaf(sib);
    free(n); free(p);
    focused = nf;
}

static void spawn(const char *cmd)
{
    if (fork() == 0) {
        setsid();
        execlp("sh", "sh", "-c", cmd, NULL);
        _exit(0);
    }
}

static unsigned long hexcolor(const char *s)
{
    return strtoul(*s == '#' ? s+1 : s, NULL, 16);
}

static void loadcfg(void)
{
    const char *paths[] = {"/etc/vex/config.conf", "./config.conf", NULL};
    FILE *f = NULL;
    for (int i = 0; paths[i] && !f; i++) f = fopen(paths[i], "r");
    if (!f) return;
    char line[512], k[64], v[448];
    while (fgets(line, sizeof line, f)) {
        if (!*line || *line == '#') continue;
        if (sscanf(line, " %63[^ =] = %447[^\n]", k, v) != 2) continue;
        if      (!strcmp(k, "gap"))           gap    = atoi(v);
        else if (!strcmp(k, "border"))        bw     = atoi(v);
        else if (!strcmp(k, "border_focus"))  cfocus = hexcolor(v);
        else if (!strcmp(k, "border_normal")) cnorm  = hexcolor(v);
        else if (!strcmp(k, "terminal"))      strncpy(term, v, sizeof(term)-1);
        else if (!strcmp(k, "bind") && nrawbinds < 32)
            strncpy(rawbinds[nrawbinds++], v, 511);
    }
    fclose(f);
}

static void processbinds(void)
{
    for (int i = 0; i < nrawbinds && nbinds < 32; i++) {
        char buf[512];
        strncpy(buf, rawbinds[i], 511);
        char *eq = strrchr(buf, '=');
        if (!eq) continue;
        *eq = 0;
        char *keys = buf, *cmd = eq+1;
        while (*cmd == ' ') cmd++;
        while (eq > keys && *(eq-1) == ' ') { eq--; *eq = 0; }

        unsigned int mask = 0;
        if (strstr(keys, "mod"))   mask |= Mod4Mask;
        if (strstr(keys, "shift")) mask |= ShiftMask;
        if (strstr(keys, "ctrl"))  mask |= ControlMask;
        if (strstr(keys, "alt"))   mask |= Mod1Mask;

        char *kp = strrchr(keys, '+');
        kp = kp ? kp+1 : keys;
        while (*kp == ' ') kp++;

        KeySym sym = XStringToKeysym(kp);
        if (sym == NoSymbol && strlen(kp) == 1) sym = (KeySym)(unsigned char)*kp;
        if (sym == NoSymbol) continue;

        KeyCode code = XKeysymToKeycode(dpy, sym);
        if (!code) continue;

        XGrabKey(dpy, code, mask, root, True, GrabModeAsync, GrabModeAsync);
        binds[nbinds].code = code;
        binds[nbinds].mod  = mask;
        strncpy(binds[nbinds].cmd, cmd, 255);
        nbinds++;
    }
}

int main(void)
{
    loadcfg();

    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;
    XSetErrorHandler(xerr);

    int scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw   = DisplayWidth(dpy, scr);
    sh   = DisplayHeight(dpy, scr);

    XSelectInput(dpy, root,
        SubstructureNotifyMask | SubstructureRedirectMask | KeyPressMask);

    unsigned int M = Mod4Mask, S = ShiftMask;
#define GRAB(sym, mask) \
    XGrabKey(dpy, XKeysymToKeycode(dpy, sym), mask, root, True, GrabModeAsync, GrabModeAsync)
    GRAB(XK_Return, M);
    GRAB(XK_q,      M);
    GRAB(XK_q,      M|S);
    GRAB(XK_Tab,    M);
    GRAB(XK_h,      M);
    GRAB(XK_l,      M);
    GRAB(XK_v,      M);
#undef GRAB

    processbinds();

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

        case ConfigureRequest: {
            XWindowChanges wc = {
                .x            = ev.xconfigurerequest.x,
                .y            = ev.xconfigurerequest.y,
                .width        = ev.xconfigurerequest.width,
                .height       = ev.xconfigurerequest.height,
                .border_width = ev.xconfigurerequest.border_width,
                .sibling      = ev.xconfigurerequest.above,
                .stack_mode   = ev.xconfigurerequest.detail,
            };
            XConfigureWindow(dpy, ev.xconfigurerequest.window,
                ev.xconfigurerequest.value_mask, &wc);
            break;
        }

        case EnterNotify:
            if (ev.xcrossing.mode == NotifyNormal) {
                Node *n = findwin(tree, ev.xcrossing.window);
                if (n) { setfocus(n); retile(); }
            }
            break;

        case KeyPress: {
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            unsigned int s = ev.xkey.state
                & (ShiftMask|ControlMask|Mod1Mask|Mod4Mask);

            int handled = 0;
            for (int i = 0; i < nbinds; i++) {
                if (ev.xkey.keycode == binds[i].code && s == binds[i].mod) {
                    spawn(binds[i].cmd);
                    handled = 1;
                    break;
                }
            }
            if (handled) break;

            if      (sym == XK_Return && s == M)     { spawn(term); }
            else if (sym == XK_q      && s == (M|S)) { return 0; }
            else if (sym == XK_q      && s == M && focused)
                XKillClient(dpy, focused->win);
            else if (sym == XK_Tab && s == M) {
                Node *n = nextleaf(focused);
                if (n) { setfocus(n); retile(); }
            } else if (sym == XK_h && s == M && focused && focused->par) {
                float r = focused->par->ratio - 0.05f;
                focused->par->ratio = r < 0.1f ? 0.1f : r;
                retile();
            } else if (sym == XK_l && s == M && focused && focused->par) {
                float r = focused->par->ratio + 0.05f;
                focused->par->ratio = r > 0.9f ? 0.9f : r;
                retile();
            } else if (sym == XK_v && s == M && focused && focused->par) {
                focused->par->horiz ^= 1;
                retile();
            }
            break;
        }

        }
    }
}
