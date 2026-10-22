/*
 * saturn - an animated ASCII Saturn for the terminal, in the spirit of momoisay.
 *
 * The scene is rendered procedurally every frame (orthographic ray cast of a
 * sphere plus a ring plane). The camera flies a full orbit around the planet on
 * a path inclined to the ring plane while the Sun stays put, so the ring opens,
 * closes edge-on and flips, the planet goes from day to night and back,
 * shadows sweep across the ring and the sky streams past. The starting pose
 * and palette are modelled on the ASCII wallpaper wallhaven-lyje9y.
 *
 * Runs on Linux, macOS and other POSIX systems, and on the Windows 10+ console.
 * Keys: q / Esc / Ctrl+C to quit.
 */
#define _DEFAULT_SOURCE
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <io.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#endif

#define VERSION "1.0.0"
#define FPS 20
#define SX 2                /* subsamples per cell, horizontal */
#define SY 4                /* subsamples per cell, vertical (cells are ~2x taller) */

/* Scene geometry, planet radius = 1; world frame has the spin axis on +Z */
#define RING_ANGLE 30.6f    /* at t = 0 the ring's major axis dips this many degrees on screen */
#define CAM_ELEV0  21.0f    /* camera elevation above the ring plane at t = 0, degrees */
#define CAM_INCL   35.0f    /* camera orbit inclination to the ring plane: the ring flips +-35 deg */
#define CAM_PERIOD 60.0f    /* seconds for the camera to fly once around the planet */
#define SUN_LEAD   0.6f     /* Sun sits this far (radians) ahead of the starting camera on its orbit */
#define SUN_LIFT   0.15f    /* ...lifted off the orbit plane so night is never a perfect eclipse */
#define FOV        80.0f    /* horizontal field of view for the star field, degrees */
#define NSTARS     1300
#define SCENE_HW   2.55f    /* half extents of the framed scene, sized for the widest pose */
#define SCENE_HH   1.65f
#define SPIN       0.40f    /* planet rotation, rad/s */
#define ORBIT      0.35f    /* ring angular speed at r = 2, rad/s (Keplerian falloff) */
#define MOON_R     0.13f    /* moon radius */
#define MOON_ORBIT 2.40f    /* moon orbit radius, just outside the A ring */
#define MOON_INCL  0.20f    /* moon orbit inclination, radians */
#define MOON_SPEED 0.45f    /* moon angular speed, rad/s */
#define METEOR_EVERY 4.0f   /* seconds per shooting-star slot */
#define TAU        6.2831853f
#define DEG        (TAU/360.0f)

static const char RAMP[] = " .:-=+*#%@";
#define NRAMP 10

typedef struct { float x, y, z; } vec3;
typedef struct { float r, g, b; } rgb;
typedef struct { char ch; uint32_t col; } cell;

/* World X, Y and Z (spin axis) seen in camera space: x right, y up, z towards the viewer */
static vec3 ax_m, ax_q, ax_n;
/* Sun direction in camera space, and the camera basis expressed in world space */
static vec3 sun, sun_w, cam_r, cam_u, cam_v;
static int truecolor;
static float cell_aspect = 0.5f;   /* character cell width / height, depends on the font */
static volatile sig_atomic_t quit;
static int term_active;

static int W, H;
static cell *cur, *prev;
static char *out;
static size_t out_len, out_cap;

static vec3 v3(float x, float y, float z) { return (vec3){x, y, z}; }
static float dot(vec3 a, vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static vec3 norm(vec3 a) { float l = sqrtf(dot(a, a)); return v3(a.x/l, a.y/l, a.z/l); }
static vec3 cross(vec3 a, vec3 b) { return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
static float lerp(float a, float b, float t) { return a + (b - a)*t; }
static float smooth(float t) { return t*t*(3 - 2*t); }

static uint32_t hash3(int x, int y, int z){
    uint32_t h = (uint32_t)x*374761393u + (uint32_t)y*668265263u + (uint32_t)z*2246822519u;
    h = (h ^ (h >> 13))*1274126177u;
    return h ^ (h >> 16);
}

static float hashf(int x, int y, int z){
    return (hash3(x, y, z) & 0xffffff)/16777215.0f;
}

static float vnoise(float x, float y, float z){
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = smooth(x - ix), fy = smooth(y - iy), fz = smooth(z - iz);
    float c[2][2];
    for(int dz=0;dz<2;dz++)
        for(int dy=0;dy<2;dy++)
            c[dz][dy] = lerp(hashf(ix, iy+dy, iz+dz), hashf(ix+1, iy+dy, iz+dz), fx);
    return lerp(lerp(c[0][0], c[0][1], fy), lerp(c[1][0], c[1][1], fy), fz);
}

static float fbm(float x, float y, float z){
    return (vnoise(x, y, z)*0.5f
          + vnoise(x*2.03f, y*2.03f, z*2.03f)*0.25f
          + vnoise(x*4.10f, y*4.10f, z*4.10f)*0.125f)/0.875f;
}

static rgb hex(uint32_t h){
    return (rgb){((h >> 16) & 255)/255.0f, ((h >> 8) & 255)/255.0f, (h & 255)/255.0f};
}

static rgb mix(rgb a, rgb b, float t){
    return (rgb){lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t)};
}

/* Accent colours sprinkled on the ring and the lit limb; weighted by repetition */
static const uint32_t ACCENTS[] = {
    0x6fe3dc, 0x6fe3dc, 0x6fe3dc, 0x6fe3dc, 0x6fe3dc, 0x6fe3dc,
    0x8c9cf0, 0x8c9cf0, 0x8c9cf0, 0x8c9cf0,
    0x7ee28c, 0x7ee28c, 0x7ee28c,
    0xe08ad8, 0xe08ad8,
    0xeef2ff, 0xeef2ff,
    0xe8dc9a,
};
#define NACCENTS (int)(sizeof(ACCENTS)/sizeof(ACCENTS[0]))

static rgb planet_color(float i){
    static const float stop[] = {0.00f, 0.25f, 0.45f, 0.65f, 0.82f, 1.00f};
    static const uint32_t col[] = {0x1c2340, 0x3b4888, 0x5b6fc4, 0x7fa8e4, 0xa6e4e2, 0xf2f6ff};
    i = clampf(i, 0, 1);
    for(int k=1;k<6;k++){
        if(i <= stop[k])return mix(hex(col[k-1]), hex(col[k]), (i - stop[k-1])/(stop[k] - stop[k-1]));
    }
    return hex(col[5]);
}

/* Camera direction (planet towards camera) at orbit angle a; the orbit plane has normal K */
static vec3 orbit_dir(float a){
    float k = CAM_INCL*DEG;
    return v3(cosf(a)*cosf(k), sinf(a), -cosf(a)*sinf(k));
}

/* Place the camera on its orbit at time t and express the world axes in camera space */
static void init_geometry(float t){
    float k = CAM_INCL*DEG;
    vec3 K = v3(sinf(k), 0, cosf(k)), Z = v3(0, 0, 1);
    float a0 = acosf(-sinf(CAM_ELEV0*DEG)/sinf(k));
    /* screen up follows the orbit normal; roll it once so the t = 0 pose matches the wallpaper */
    vec3 r0 = cross(K, orbit_dir(a0));
    float roll = RING_ANGLE*DEG - atan2f(dot(Z, r0), dot(Z, K));
    float cr = cosf(roll), sr = sinf(roll);
    cam_v = orbit_dir(a0 + TAU*t/CAM_PERIOD);
    vec3 rt = cross(K, cam_v);
    cam_r = v3(cr*rt.x + sr*K.x, cr*rt.y + sr*K.y, cr*rt.z + sr*K.z);
    cam_u = v3(cr*K.x - sr*rt.x, cr*K.y - sr*rt.y, cr*K.z - sr*rt.z);
    ax_m = v3(cam_r.x, cam_u.x, cam_v.x);
    ax_q = v3(cam_r.y, cam_u.y, cam_v.y);
    ax_n = v3(dot(Z, cam_r), dot(Z, cam_u), dot(Z, cam_v));
    vec3 sd = orbit_dir(a0 + SUN_LEAD);
    sun_w = norm(v3(sd.x + K.x*SUN_LIFT, sd.y + K.y*SUN_LIFT, sd.z + K.z*SUN_LIFT));
    sun = v3(dot(sun_w, cam_r), dot(sun_w, cam_u), dot(sun_w, cam_v));
}

/* Saturn's main rings in planet radii: C ring, B ring, Cassini division, A ring with the
   Encke gap. tau is the normal optical depth, alb how bright the particles are. */
typedef struct { float tau, alb; rgb col; } ringpt;

static int ring_at(float r, ringpt *o){
    if(r < 1.24f || r > 2.27f)return 0;
    float fine = vnoise(r*18, 0.5f, 0.5f)*0.6f + vnoise(r*45, 1.5f, 0.5f)*0.4f;   /* ringlets */
    if(r < 1.53f){
        o->tau = 0.10f*(0.5f + fine);
        o->alb = 0.40f;
        o->col = hex(0x4a5690);
    }
    else if(r < 1.95f){
        float in = smooth(clampf((r - 1.53f)/0.12f, 0, 1));   /* B ring brightens past its inner edge */
        o->tau = (0.4f + 1.4f*in)*(0.8f + 0.4f*fine);
        o->alb = (0.42f + 0.30f*in)*(0.85f + 0.3f*fine);
        o->col = mix(hex(0x8ea6e8), hex(0xcfe6f4), in*fine);
    }
    else if(r < 2.03f){
        o->tau = 0.06f*(0.5f + fine);
        o->alb = 0.35f;
        o->col = hex(0x3a4478);
    }
    else if(r > 2.205f && r < 2.215f){
        o->tau = 0.01f;
        o->alb = 0.30f;
        o->col = hex(0x3a4478);
    }
    else{
        o->tau = 0.55f*(0.75f + 0.5f*fine);
        o->alb = 0.55f*(0.85f + 0.3f*fine);
        o->col = mix(hex(0x7f8fe0), hex(0xa8bcf0), fine);
    }
    return 1;
}

typedef struct { float bands, turb, storm, bw, ru; } surface;

/* Cloud bands and storms at the planet point under world (x, y), in the spinning frame */
static surface planet_surface(float x, float y, float t){
    surface sf = {0, 0, 0, 0, 0};
    float r2 = x*x + y*y;
    if(r2 > 0.998f){
        float k = sqrtf(0.998f/r2);
        x *= k; y *= k; r2 = 0.998f;
    }
    vec3 N = v3(x, y, sqrtf(1.0f - r2));
    float bu = dot(N, ax_m), bv = dot(N, ax_q), bw = dot(N, ax_n);
    float a = SPIN*t, ca = cosf(a), sa = sinf(a);
    float ru = bu*ca - bv*sa, rv = bu*sa + bv*ca;
    float warp = fbm(ru*2.2f, bw*5.0f, rv*2.2f);
    sf.bands = 0.5f + 0.5f*sinf(bw*18.0f + warp*2.5f);
    sf.turb = fbm(ru*5.0f + 7, bw*11.0f, rv*5.0f);
    /* storms fixed in the rotating frame, so they drift across the disc */
    static const float storms[][4] = {
        { 0.947f,  0.000f, -0.32f, 70},
        {-0.571f,  0.786f,  0.25f, 90},
        { 0.120f, -0.930f, -0.05f, 60},
        {-0.700f, -0.640f, -0.30f, 110},
        { 0.560f,  0.770f,  0.42f, 120},
    };
    sf.storm = 0;
    for(int k=0;k<5;k++){
        const float *st = storms[k];
        float d = (ru*st[0] + rv*st[1] + bw*st[2])/sqrtf(st[0]*st[0] + st[1]*st[1] + st[2]*st[2]);
        sf.storm += expf(-(1 - d)*st[3]);
    }
    sf.bw = bw;
    sf.ru = ru;
    return sf;
}

/* Shade one subsample at screen-world (x, y); returns intensity, writes colour */
static float shade(float x, float y, float t, const surface *sf, rgb *c){
    float r2 = x*x + y*y;
    int on_planet = r2 < 1.0f;
    float zp = on_planet ? sqrtf(1.0f - r2) : -1e9f;
    float ring_i = 0, ring_a = 0, pl_i = 0;
    rgb ring_c = {0, 0, 0}, pl_c = {0, 0, 0};

    float ma = MOON_SPEED*t, mc = cosf(ma)*MOON_ORBIT, ms = sinf(ma)*MOON_ORBIT;
    float mq = ms*cosf(MOON_INCL), mn = ms*sinf(MOON_INCL);
    vec3 mo = v3(ax_m.x*mc + ax_q.x*mq + ax_n.x*mn, ax_m.y*mc + ax_q.y*mq + ax_n.y*mn, ax_m.z*mc + ax_q.z*mq + ax_n.z*mn);
    float mdx = (x - mo.x)/MOON_R, mdy = (y - mo.y)/MOON_R, md2 = mdx*mdx + mdy*mdy;
    float zm = md2 < 1 ? mo.z + MOON_R*sqrtf(1 - md2) : -1e9f;
    int on_moon = zm > zp;
    float zb = on_moon ? zm : zp;   /* depth of the nearest solid surface */

    /* near edge-on the ring plane collapses to a line; keep the intersection finite */
    float nz = fabsf(ax_n.z) < 0.03f ? (ax_n.z < 0 ? -0.03f : 0.03f) : ax_n.z;
    float zr = -(ax_n.x*x + ax_n.y*y)/nz;
    vec3 p = v3(x, y, zr);
    float u = dot(p, ax_m), w = dot(p, ax_q);
    float rr = sqrtf(u*u + w*w);
    ringpt rp;
    if(zr > zb && ring_at(rr, &rp)){
        /* faint clumps carried round at the Keplerian rate show the ring turning */
        float phi = atan2f(w, u) + ORBIT*t*powf(rr*0.5f, -1.5f);
        float clump = vnoise(cosf(phi)*9 + 11, sinf(phi)*9, rr*14)*0.65f + vnoise(cosf(phi)*23, sinf(phi)*23, rr*30)*0.35f;
        float turn = phi/TAU;
        int seg = (int)floorf((turn - floorf(turn))*220);
        int band = (int)(rr*40);
        /* opacity along the line of sight grows as the view grazes the ring plane,
           so an edge-on ring closes into a solid line */
        ring_a = 1 - expf(-rp.tau/fabsf(nz));
        /* the lit face reflects sunlight; from the unlit face only light diffusing
           through the thinner parts shows, and that glows when the Sun is behind */
        float back = fmaxf(0, -sun.z);
        int lit_face = (dot(sun, ax_n) > 0) == (ax_n.z > 0);
        float diffuse = rp.tau*expf(-2.5f*rp.tau)*6.0f;   /* peaks for thin rings, the dense B ring stays dark */
        float light = lit_face ? 0.55f + 0.30f*sun.z + 0.35f*back : 0.10f + diffuse*(0.35f + 0.8f*back);
        ring_i = rp.alb*light*(0.75f + 0.5f*clump);
        ring_c = rp.col;
        if(hashf(seg, band, 7) > 0.975f){
            ring_i += 0.3f;
            ring_c = mix(ring_c, hex(ACCENTS[hash3(seg, band, 9) % NACCENTS]), 0.7f);
        }
        /* planet shadow: the ray to the Sun passes through the sphere */
        float qs = dot(p, sun);
        if(qs < 0 && dot(p, p) - qs*qs < 1)ring_i *= 0.06f;
        ring_i = clampf(ring_i, 0, 1);
    }

    if(on_moon){
        vec3 N = v3(mdx, mdy, (zm - mo.z)/MOON_R);
        pl_i = clampf(0.10f + 0.85f*fmaxf(0, dot(N, sun)), 0, 1);
        pl_c = mix(hex(0x6c6a9c), hex(0xf4eedc), pl_i);
    }
    else if(on_planet){
        vec3 N = v3(x, y, zp);
        float ndl = dot(N, sun);
        float day = clampf(ndl*1.4f + 0.1f, 0, 1);
        /* ring shadow: follow the ray to the Sun up to the ring plane */
        float shadow = 1;
        float sn = dot(sun, ax_n);
        float s = fabsf(sn) > 1e-3f ? -dot(N, ax_n)/sn : -1;
        ringpt sp;
        if(s > 0){
            vec3 q = v3(N.x + sun.x*s, N.y + sun.y*s, N.z + sun.z*s);
            if(ring_at(sqrtf(dot(q, q)), &sp))shadow = 0.15f + 0.85f*expf(-sp.tau/fabsf(sn));
        }
        float lit = day*shadow;
        /* thin atmosphere: a bright edge mostly when the Sun is behind the planet */
        float limb = (1 - zp)*(1 - zp)*(1 - zp)*clampf(ndl + 0.35f, 0, 1)*(0.3f + 1.2f*fmaxf(0, -sun.z));
        pl_i = 0.09f + 0.05f*sf->bands
             + lit*(0.44f*(0.65f + 0.35f*sf->bands) + 0.26f*(sf->turb - 0.5f) + 0.30f*sf->storm)
             + 0.9f*limb;
        pl_i = clampf(pl_i, 0, 1);
        pl_c = planet_color(fmaxf(pl_i, 0.24f));
        if(limb > 0.10f)pl_c = mix(pl_c, hex(ACCENTS[hash3((int)(sf->bw*30), (int)(sf->ru*6), 1) % NACCENTS]), 0.6f);
    }

    if(on_planet || on_moon){
        float i = ring_i*ring_a + pl_i*(1 - ring_a);
        float wr = ring_i*ring_a, wp = pl_i*(1 - ring_a), ws = wr + wp;
        if(ws > 0)*c = mix(pl_c, ring_c, wr/ws);
        return i;
    }
    *c = ring_c;
    return ring_i*ring_a;
}

static uint32_t pack(rgb c, float scale){
    int r = (int)(clampf(c.r*scale, 0, 1)*255), g = (int)(clampf(c.g*scale, 0, 1)*255), b = (int)(clampf(c.b*scale, 0, 1)*255);
    return ((uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b) & 0xfcfcfc;
}

/* A shooting star per time slot (some slots stay empty), drawn only over empty sky */
static void meteor(float t){
    int slot = (int)floorf(t/METEOR_EVERY);
    float age = t - slot*METEOR_EVERY, life = 1.1f;
    if(age > life || hashf(slot, 0, 77) < 0.25f)return;
    float sx = (0.25f + 0.70f*hashf(slot, 1, 77))*W, sy = (0.02f + 0.30f*hashf(slot, 2, 77))*H;
    float ang = 0.30f + 0.35f*hashf(slot, 3, 77);           /* radians below horizontal, heading left */
    float dx = -cosf(ang), dy = sinf(ang)*cell_aspect;
    float dist = age/life*W*0.45f;
    static const char trail[] = "@*==---::::.....";
    for(int k=0;k<(int)sizeof(trail) - 1;k++){
        float d = dist - k*1.4f;
        if(d < 0)break;
        int x = (int)(sx + dx*d), y = (int)(sy + dy*d);
        if(x < 0 || x >= W || y < 0 || y >= H)continue;
        cell *cl = &cur[y*W + x];
        if(cl->ch != ' ' && cl->ch != '.')continue;
        float fade = (1 - k/16.0f)*(1 - 0.5f*age/life);
        cl->ch = trail[k];
        cl->col = pack(mix(hex(0x6fe3dc), hex(0xffffff), 1 - k/10.0f), fade);
    }
}

/* Project a sky direction (world space) to a cell; returns 0 if outside the view */
static int project_sky(vec3 d, float *sx, float *sy){
    float f = -dot(d, cam_v);
    if(f < 0.2f)return 0;
    float F = W*0.5f/tanf(FOV*0.5f*DEG);
    *sx = W*0.5f + dot(d, cam_r)/f*F;
    *sy = H*0.5f - dot(d, cam_u)/f*F*cell_aspect;
    return *sx >= 0 && *sx < W && *sy >= 0 && *sy < H;
}

/* Stars and the Sun live on the celestial sphere, so they stream past as the camera orbits */
static void sky(float t){
    float sx, sy;
    if(project_sky(sun_w, &sx, &sy)){
        static const char glow[] = "@#*+:.";
        for(int dy=-3;dy<=3;dy++){
            for(int dx=-7;dx<=7;dx++){
                int x = (int)sx + dx, y = (int)sy + dy;
                if(x < 0 || x >= W || y < 0 || y >= H || cur[y*W + x].ch != ' ')continue;
                float d = sqrtf(dx*dx*0.25f + dy*dy)*1.3f;
                if(d >= 6)continue;
                cur[y*W + x] = (cell){glow[(int)d], pack(mix(hex(0xfff6d8), hex(0xe8b860), d/6), 1 - d/7)};
            }
        }
    }
    for(int k=0;k<NSTARS;k++){
        float z = hashf(k, 1, 5)*2 - 1, a = hashf(k, 2, 5)*TAU, rxy = sqrtf(1 - z*z);
        if(!project_sky(v3(rxy*cosf(a), rxy*sinf(a), z), &sx, &sy))continue;
        cell *cl = &cur[(int)sy*W + (int)sx];
        if(cl->ch != ' ')continue;
        uint32_t h = hash3(k, 3, 5);
        float ph = (h & 255)/255.0f*TAU;
        float sp = 1.0f + ((h >> 8) & 15)/15.0f*3.0f;
        float b = 0.30f + 0.70f*(0.5f + 0.5f*sinf(t*sp + ph));
        int kind = (h >> 12) % 12;
        rgb sc = kind == 0 ? hex(ACCENTS[(h >> 16) % NACCENTS]) : hex(0x8a90a8);
        cl->ch = kind < 2 ? (kind == 0 ? '=' : ':') : '.';
        cl->col = pack(sc, b);
    }
}

static void render(float t){
    float S = fminf(W/(2*SCENE_HW), H/(cell_aspect*2*SCENE_HH));
    init_geometry(t);
    for(int row=0;row<H;row++){
        for(int col=0;col<W;col++){
            float sum = 0;
            rgb acc = {0, 0, 0};
            float xc = (col + 0.5f - W*0.5f)/S, yc = -(row + 0.5f - H*0.5f)/(S*cell_aspect);
            surface sf = {0, 0, 0, 0, 0};
            if(xc*xc + yc*yc < 1.2f)sf = planet_surface(xc, yc, t);
            for(int j=0;j<SY;j++){
                for(int i=0;i<SX;i++){
                    float x = (col + (i + 0.5f)/SX - W*0.5f)/S;
                    float y = -(row + (j + 0.5f)/SY - H*0.5f)/(S*cell_aspect);
                    rgb c;
                    float v = shade(x, y, t, &sf, &c);
                    if(v > 0){
                        sum += v;
                        acc.r += c.r*v; acc.g += c.g*v; acc.b += c.b*v;
                    }
                }
            }
            float avg = sum/(SX*SY);
            int idx = (int)(powf(avg, 0.8f)*NRAMP);
            if(idx > NRAMP - 1)idx = NRAMP - 1;
            cell *cl = &cur[row*W + col];
            if(idx > 0){
                cl->ch = RAMP[idx];
                cl->col = pack((rgb){acc.r/sum, acc.g/sum, acc.b/sum}, 1.0f);
            }
            else{
                cl->ch = ' ';
                cl->col = 0;
            }
        }
    }
    sky(t);
    meteor(t);
}

static void put(const char *s, size_t n){
    if(out_len + n > out_cap){
        out_cap = (out_len + n)*2;
        out = realloc(out, out_cap);
    }
    memcpy(out + out_len, s, n);
    out_len += n;
}

static void putf(const char *fmt, int a, int b, int c){
    char buf[32];
    int n = snprintf(buf, sizeof buf, fmt, a, b, c);
    put(buf, (size_t)n);
}

static void set_color(uint32_t col){
    int r = col >> 16 & 255, g = col >> 8 & 255, b = col & 255;
    if(truecolor)putf("\x1b[38;2;%d;%d;%dm", r, g, b);
    else putf("\x1b[38;5;%dm", 16 + 36*((r*5 + 127)/255) + 6*((g*5 + 127)/255) + (b*5 + 127)/255, 0, 0);
}

/* Emit only the cells that changed since the previous frame */
static void flush(int full){
    out_len = 0;
    put("\x1b[?2026h", 8);
    if(full)put("\x1b[0m\x1b[2J", 8);
    int cx = -1, cy = -1;
    uint32_t last = 0xffffffff;
    for(int y=0;y<H;y++){
        for(int x=0;x<W;x++){
            cell c = cur[y*W + x], p = prev[y*W + x];
            if(full ? c.ch == ' ' : (c.ch == p.ch && c.col == p.col))continue;
            if(cy != y || cx != x)putf("\x1b[%d;%dH", y + 1, x + 1, 0);
            if(c.ch != ' ' && c.col != last){
                set_color(c.col);
                last = c.col;
            }
            put(&c.ch, 1);
            cx = x + 1; cy = y;
        }
    }
    put("\x1b[?2026l", 8);
    fwrite(out, 1, out_len, stdout);
    fflush(stdout);
}

static void resize(int w, int h){
    W = w; H = h;
    free(cur); free(prev);
    cur = calloc((size_t)(W*H), sizeof(cell));
    prev = calloc((size_t)(W*H), sizeof(cell));
}

/* ---- terminal platform layer: POSIX (Linux, macOS, BSD) and the Windows 10+ console ---- */
#ifdef _WIN32
static HANDLE h_out, h_in;
static DWORD old_out_mode, old_in_mode;

static BOOL WINAPI on_ctrl(DWORD type){
    (void)type;
    quit = 1;
    return TRUE;
}

static int term_is_tty(void){
    return _isatty(_fileno(stdout));
}

/* Turn on ANSI escape processing and unbuffered key input; fails before Windows 10 */
static int term_setup(void){
    h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    h_in = GetStdHandle(STD_INPUT_HANDLE);
    if(!GetConsoleMode(h_out, &old_out_mode))return 0;
    if(!SetConsoleMode(h_out, old_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING))return 0;
    if(GetConsoleMode(h_in, &old_in_mode))
        SetConsoleMode(h_in, old_in_mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    return 1;
}

static void term_reset_modes(void){
    SetConsoleMode(h_out, old_out_mode);
    SetConsoleMode(h_in, old_in_mode);
}

static int term_size(int *w, int *h){
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if(!GetConsoleScreenBufferInfo(h_out, &ci))return 0;
    *w = ci.srWindow.Right - ci.srWindow.Left + 1;
    *h = ci.srWindow.Bottom - ci.srWindow.Top + 1;
    return 1;
}

static double now(void){
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if(!freq.QuadPart)QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart/(double)freq.QuadPart;
}

/* Sleep up to ms, then drain the keyboard; returns 1 when a quit key was pressed */
static int term_wait_key(int ms){
    if(ms > 0)Sleep((DWORD)ms);
    while(_kbhit()){
        int c = _getch();
        if(c == 0 || c == 0xE0){
            _getch();   /* arrow and function keys arrive as a two-byte sequence */
            continue;
        }
        if(c == 'q' || c == 'Q' || c == 27)return 1;
    }
    return 0;
}
#else
static struct termios saved_tio;
static int tio_saved, stdin_eof;

static void on_signal(int sig){
    (void)sig;
    quit = 1;
}

static int term_is_tty(void){
    return isatty(STDOUT_FILENO);
}

/* Catch Ctrl+C and friends, and read keys without waiting for Enter or echoing them */
static int term_setup(void){
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    if(tcgetattr(STDIN_FILENO, &saved_tio) == 0){
        struct termios raw = saved_tio;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        tio_saved = 1;
    }
    return 1;
}

static void term_reset_modes(void){
    if(tio_saved)tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
    tio_saved = 0;
}

static int term_size(int *w, int *h){
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0)return 0;
    *w = ws.ws_col;
    *h = ws.ws_row;
    return 1;
}

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec*1e-9;
}

/* Wait up to ms for input; returns 1 when a quit key was pressed */
static int term_wait_key(int ms){
    /* once stdin hits EOF (e.g. < /dev/null) it is always "readable", so just sleep */
    struct pollfd pfd = {stdin_eof ? -1 : STDIN_FILENO, POLLIN, 0};
    if(poll(&pfd, 1, ms) <= 0 || !(pfd.revents & POLLIN))return 0;
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
    if(n == 0)stdin_eof = 1;
    for(ssize_t i=0;i<n;i++){
        if(buf[i] == 'q' || buf[i] == 'Q' || buf[i] == 27 || buf[i] == 3)return 1;
    }
    return 0;
}
#endif

static void restore_terminal(void){
    if(!term_active)return;
    fputs("\x1b[0m\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    term_reset_modes();
    term_active = 0;
}

/* 24-bit colour unless the terminal is known not to have it; SATURN_COLORS overrides */
static int detect_truecolor(void){
    const char *e = getenv("SATURN_COLORS");
    if(e && *e)return strcmp(e, "256") != 0;
    const char *ct = getenv("COLORTERM");
    if(ct && (strstr(ct, "truecolor") || strstr(ct, "24bit")))return 1;
#ifdef _WIN32
    return 1;   /* both the Windows 10+ console and Windows Terminal render 24-bit colour */
#else
    const char *tp = getenv("TERM_PROGRAM");
    return tp && (!strcmp(tp, "iTerm.app") || !strcmp(tp, "WezTerm") || !strcmp(tp, "vscode"));
#endif
}

static void help(){
    printf(
        "Animated ASCII Saturn for your terminal.\n"
        "usage: saturn [options]\n"
        "    -h, --help                Display this help message\n"
        "    -v, --version             Show version information\n"
        "    -s, --speed <x>           Animation speed multiplier (default 1)\n"
        "    -c, --colors <mode>       Force colour mode: truecolor or 256\n"
        "    -a, --aspect <ratio>      Font cell width/height if the planet looks squashed (default 0.5)\n"
        "    --dump <COLSxROWS> <sec>  Print a single frame and exit\n"
        "keys: q, Esc or Ctrl+C to quit\n"
    );
}

static void dump(int w, int h, float t){
    resize(w, h);
    render(t);
    for(int y=0;y<H;y++){
        for(int x=0;x<W;x++){
            cell c = cur[y*W + x];
            if(c.ch != ' ')set_color(c.col);
            put(&c.ch, 1);
        }
        put("\x1b[0m\n", 5);
    }
    fwrite(out, 1, out_len, stdout);
}

int main(int argc, char *argv[]){
    truecolor = detect_truecolor();
    float speed = 1.0f;
    int dump_w = 0, dump_h = 0;
    float dump_t = 0;

    for(int i=1;i<argc;i++){
        const char *a = argv[i];
        if(!strcmp(a, "-h") || !strcmp(a, "--help")){
            help();
            return 0;
        }
        else if(!strcmp(a, "-v") || !strcmp(a, "--version")){
            printf("saturn v%s\n", VERSION);
            return 0;
        }
        else if((!strcmp(a, "-s") || !strcmp(a, "--speed")) && i + 1 < argc){
            speed = (float)atof(argv[++i]);
            if(speed <= 0){
                fprintf(stderr, "saturn: speed must be a positive number\n");
                return 1;
            }
        }
        else if((!strcmp(a, "-c") || !strcmp(a, "--colors")) && i + 1 < argc){
            const char *m = argv[++i];
            if(strcmp(m, "truecolor") && strcmp(m, "256")){
                fprintf(stderr, "saturn: colour mode must be truecolor or 256\n");
                return 1;
            }
            truecolor = strcmp(m, "256") != 0;
        }
        else if((!strcmp(a, "-a") || !strcmp(a, "--aspect")) && i + 1 < argc){
            cell_aspect = (float)atof(argv[++i]);
            if(cell_aspect < 0.2f || cell_aspect > 1.5f){
                fprintf(stderr, "saturn: aspect must be between 0.2 and 1.5\n");
                return 1;
            }
        }
        else if(!strcmp(a, "--dump") && i + 2 < argc){
            if(sscanf(argv[i + 1], "%dx%d", &dump_w, &dump_h) != 2 || dump_w < 1 || dump_h < 1){
                fprintf(stderr, "saturn: --dump size must look like 120x40\n");
                return 1;
            }
            dump_t = (float)atof(argv[i + 2]);
            i += 2;
        }
        else{
            help();
            return 1;
        }
    }

    if(dump_w){
        dump(dump_w, dump_h, dump_t*speed);
        return 0;
    }
    if(!term_is_tty()){
        fprintf(stderr, "saturn: stdout is not a terminal\n");
        return 1;
    }
    if(!term_setup()){
        fprintf(stderr, "saturn: this console has no ANSI escape support (Windows 10 or newer is required)\n");
        return 1;
    }
    term_active = 1;
    atexit(restore_terminal);
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);

    double t0 = now(), next = t0;
    while(!quit){
        int w, h, full = 0;
        if(term_size(&w, &h) && w > 0 && h > 0 && (w != W || h != H)){
            resize(w, h);
            full = 1;
        }
        if(!W)break;
        render((float)(now() - t0)*speed);
        flush(full);
        cell *tmp = prev; prev = cur; cur = tmp;

        next += 1.0/FPS;
        double wait = next - now();
        if(wait < 0){
            next = now();
            wait = 0;
        }
        if(term_wait_key((int)(wait*1000)))quit = 1;
    }
    restore_terminal();
    return 0;
}
