#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TOP_W 400
#define TOP_H 240

// Simple 3D Vector & Color helpers for pure software 3D rendering on 2DS
struct Vec3 { float x, y, z; };
struct Point2D { int x, y; bool visible; };

struct Vehicle {
    float x, z;
    float angle;
    float speed;
    float damage;
};

// Draw a filled pixel safely on the 2DS Top Screen framebuffer
inline void putPixel(u8* fb, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= TOP_W || y < 0 || y >= TOP_H) return;
    // 3DS top screen framebuffer is rotated 90 degrees counter-clockwise
    int idx = ((x * TOP_H) + (TOP_H - 1 - y)) * 3;
    fb[idx + 0] = b;
    fb[idx + 1] = g;
    fb[idx + 2] = r;
}

// Draw a fast 2D line between two projected 3D points
void drawLine(u8* fb, int x0, int y0, int x1, int y1, u8 r, u8 g, u8 b) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        putPixel(fb, x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Project a 3D world coordinate onto the 2DS 400x240 screen
Point2D project3D(Vec3 p, Vec3 camPos, float camYaw) {
    float dx = p.x - camPos.x;
    float dy = p.y - camPos.y;
    float dz = p.z - camPos.z;

    // Rotate around camera Y axis
    float cosY = cosf(-camYaw);
    float sinY = sinf(-camYaw);
    float rx = dx * cosY - dz * sinY;
    float rz = dx * sinY + dz * cosY;

    if (rz <= 0.5f) return { 0, 0, false }; // Behind camera

    float fov = 260.0f;
    int sx = (int)(TOP_W / 2 + (rx * fov) / rz);
    int sy = (int)(TOP_H / 2 - (dy * fov) / rz);
    return { sx, sy, true };
}

// Draw a 3D wireframe/solid-edged box in the world
void draw3DBox(u8* fb, Vec3 center, Vec3 size, float rotY, Vec3 camPos, float camYaw, u8 r, u8 g, u8 b) {
    float hx = size.x * 0.5f, hy = size.y * 0.5f, hz = size.z * 0.5f;
    Vec3 local[8] = {
        {-hx, -hy, -hz}, { hx, -hy, -hz}, { hx,  hy, -hz}, {-hx,  hy, -hz},
        {-hx, -hy,  hz}, { hx, -hy,  hz}, { hx,  hy,  hz}, {-hx,  hy,  hz}
    };

    Point2D proj[8];
    float c = cosf(rotY), s = sinf(rotY);
    for (int i = 0; i < 8; i++) {
        Vec3 world = {
            center.x + (local[i].x * c + local[i].z * s),
            center.y + local[i].y,
            center.z + (-local[i].x * s + local[i].z * c)
        };
        proj[i] = project3D(world, camPos, camYaw);
    }

    int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, // Back
        {4,5},{5,6},{6,7},{7,4}, // Front
        {0,4},{1,5},{2,6},{3,7}  // Sides
    };

    for (int i = 0; i < 12; i++) {
        Point2D p1 = proj[edges[i][0]];
        Point2D p2 = proj[edges[i][1]];
        if (p1.visible && p2.visible) {
            drawLine(fb, p1.x, p1.y, p2.x, p2.y, r, g, b);
        }
    }
}

int main(int argc, char* argv[]) {
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, NULL);

    Vehicle player = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    Vehicle cop    = { 0.0f, -22.0f, 0.0f, 0.0f, 0.0f };
    int felony = 1;
    int sirenTick = 0;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;
        if (kDown & KEY_X) {
            player = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            cop    = { 0.0f, -22.0f, 0.0f, 0.0f, 0.0f };
            felony = 1;
        }

        // Circle Pad & D-Pad Steering
        circlePosition pos;
        hidCircleRead(&pos);
        float steer = 0.0f;
        if (abs(pos.dx) > 15) steer = (float)pos.dx / 156.0f;
        if (kHeld & KEY_LEFT)  steer = -1.0f;
        if (kHeld & KEY_RIGHT) steer =  1.0f;

        // Gas (A), Brake/Reverse (B), Handbrake Drift (R)
        float accel = 0.0f;
        if (kHeld & KEY_A) accel =  0.025f;
        if (kHeld & KEY_B) accel = -0.020f;

        bool drifting = (kHeld & KEY_R) != 0;
        float turnRate = drifting ? 0.068f : 0.044f;
        float grip     = drifting ? 0.985f : 0.960f;

        player.speed = (player.speed + accel) * grip;
        if (fabsf(player.speed) > 0.01f) {
            float rev = (player.speed >= 0) ? 1.0f : -1.0f;
            player.angle += steer * turnRate * rev;
        }

        float nextX = player.x + sinf(player.angle) * player.speed;
        float nextZ = player.z + cosf(player.angle) * player.speed;

        // City Block Building Collisions
        bool hit = false;
        for (int bx = -2; bx <= 2; bx++) {
            for (int bz = -2; bz <= 2; bz++) {
                float buildX = bx * 26.0f + 13.0f;
                float buildZ = bz * 26.0f + 13.0f;
                if (fabsf(nextX - buildX) < 7.5f && fabsf(nextZ - buildZ) < 7.5f) {
                    hit = true;
                }
            }
        }

        if (hit) {
            player.damage += fabsf(player.speed) * 35.0f;
            if (player.damage > 100.0f) player.damage = 100.0f;
            player.speed = -player.speed * 0.45f;
            if (felony < 5) felony++;
        } else {
            player.x = nextX;
            player.z = nextZ;
        }

        // Cop Car AI Pursuit
        float dx = player.x - cop.x;
        float dz = player.z - cop.z;
        float dist = sqrtf(dx * dx + dz * dz);
        cop.angle = atan2f(dx, dz);
        if (dist > 3.5f) {
            float copSpeed = 0.42f + (felony * 0.03f);
            cop.x += sinf(cop.angle) * copSpeed;
            cop.z += cosf(cop.angle) * copSpeed;
        } else {
            player.damage += 0.35f;
            if (player.damage > 100.0f) player.damage = 100.0f;
        }

        // Clear Top Screen Framebuffer (Sky top, Dark Asphalt bottom)
        u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
        memset(fb, 35, TOP_W * TOP_H * 3);

        // Chase Camera behind Player Car
        Vec3 camPos = {
            player.x - sinf(player.angle) * 9.0f,
            3.8f,
            player.z - cosf(player.angle) * 9.0f
        };

        // Draw 3D Street Grid Lines
        for (int g = -60; g <= 60; g += 12) {
            Point2D a1 = project3D({(float)g, 0.0f, -60.0f}, camPos, player.angle);
            Point2D a2 = project3D({(float)g, 0.0f,  60.0f}, camPos, player.angle);
            if (a1.visible && a2.visible) drawLine(fb, a1.x, a1.y, a2.x, a2.y, 70, 70, 75);

            Point2D b1 = project3D({-60.0f, 0.0f, (float)g}, camPos, player.angle);
            Point2D b2 = project3D({ 60.0f, 0.0f, (float)g}, camPos, player.angle);
            if (b1.visible && b2.visible) drawLine(fb, b1.x, b1.y, b2.x, b2.y, 70, 70, 75);
        }

        // Draw 3D City Skyscrapers
        for (int bx = -2; bx <= 2; bx++) {
            for (int bz = -2; bz <= 2; bz++) {
                float buildX = bx * 26.0f + 13.0f;
                float buildZ = bz * 26.0f + 13.0f;
                float h = 10.0f + (abs(bx * 7 + bz * 13) % 14);
                draw3DBox(fb, {buildX, h * 0.5f, buildZ}, {12.0f, h, 12.0f}, 0.0f, camPos, player.angle, 90, 180, 220);
            }
        }

        // Draw Player Muscle Car (Blue/Cyan)
        draw3DBox(fb, {player.x, 0.55f, player.z}, {1.8f, 0.7f, 3.8f}, player.angle, camPos, player.angle, 50, 200, 255);
        draw3DBox(fb, {player.x, 1.10f, player.z}, {1.4f, 0.5f, 2.0f}, player.angle, camPos, player.angle, 120, 230, 255);

        // Draw Police Cruiser + Flashing Sirens
        sirenTick++;
        bool redFlash = ((sirenTick / 8) % 2) == 0;
        draw3DBox(fb, {cop.x, 0.55f, cop.z}, {1.8f, 0.7f, 3.8f}, cop.angle, camPos, player.angle, 240, 240, 240);
        draw3DBox(fb, {cop.x, 1.35f, cop.z}, {0.9f, 0.25f, 0.3f}, cop.angle, camPos, player.angle,
                  redFlash ? 255 : 30, 30, redFlash ? 30 : 255);

        // Bottom Screen HUD
        printf("\x1b[2;2H=== REDRIVER2 2DS (SINGLE FILE) ===");
        printf("\x1b[4;2HSpeed:   %3.0f MPH   ", fabsf(player.speed) * 145.0f);
        printf("\x1b[5;2HDamage:  %3.0f%%      ", player.damage);
        printf("\x1b[6;2HFelony:  ");
        for (int f = 0; f < 5; f++) printf(f < felony ? "[*]" : "[ ]");

        printf("\x1b[9;2HControls:");
        printf("\x1b[10;2H - Circle Pad / D-Pad: Steer");
        printf("\x1b[11;2H - A: Gas  |  B: Brake / Reverse");
        printf("\x1b[12;2H - R: Handbrake Drift");
        printf("\x1b[13;2H - X: Repair Car & Reset");
        printf("\x1b[14;2H - START: Exit");

        if (player.damage >= 100.0f) {
            printf("\x1b[17;2H[!] CAR WRECKED! Press X to Reset");
        } else {
            printf("\x1b[17;2HStatus: Evade the Police!        ");
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
    return 0;
}
