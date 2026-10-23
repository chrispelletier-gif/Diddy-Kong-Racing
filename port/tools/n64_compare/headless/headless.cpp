// Minimal headless ares frontend for the Nintendo 64, for comparing the port's renderer with the real thing (see
// ../README.md). Runs a ROM with scripted input and, for chosen graphics tasks, saves RDRAM as the task starts
// (task_N.rdram.gz) and the colour image as the RDP finishes it (sync_N_S.fb). Can also write the VI output (PPM).
//
//   ares-headless --rom dkr.z64 --out dir [--frames n] [--script "frame:BUTTONS ..."] [--video-every n]
//                 [--dump-tasks a,b,c] [--dump-every n --dump-from n [--dump-to n]] [--no-gpu] [--no-vi]
//
// Needs the hooks in ../ares-hooks.patch (spStartHook, rdpSyncHook).
#include <zlib.h>
#include <n64/n64.hpp>
#include <mia/mia.hpp>
#include <nall/main.hpp>

using namespace nall;

static shared_pointer<mia::Pak> gSystem, gGame;
static ares::Node::System gRoot;
static u32 gViFrame = 0;
static u32 gGfxTask = 0;
static string gOutDir = "out";
static vector<u32> gDumpTasks;   // gfx task numbers to dump
static u32 gDumpEvery = 0, gDumpFrom = 0, gDumpTo = 0;
static u32 gVideoEvery = 0;
struct Step { u32 frame; u32 buttons; s32 x, y; };
static vector<Step> gScript;
static u32 gButtons = 0; static s32 gX = 0, gY = 0;

enum { B_A=1, B_B=2, B_Z=4, B_START=8, B_UP=16, B_DOWN=32, B_LEFT=64, B_RIGHT=128, B_L=256, B_R=512,
       B_CU=1024, B_CD=2048, B_CL=4096, B_CR=8192 };

static void write_gz(const string& path, const u8* data, u32 size) {
  gzFile f = gzopen(path, "wb1");
  gzwrite(f, data, size);
  gzclose(f);
}

static bool shouldDump(u32 id) {
  if(gDumpTasks.find(id)) return true;
  if(gDumpEvery && id >= gDumpFrom && (!gDumpTo || id <= gDumpTo) && (id - gDumpFrom) % gDumpEvery == 0) return true;
  return false;
}
static u32 gSyncSeq = 0;
static void syncHook(u32 cimg, u32 width) {
  using namespace ares::Nintendo64;
  gSyncSeq++;
  if(getenv("SYNC_LOG")) print("sync ", gSyncSeq, " after task ", gGfxTask, " cimg ", hex(cimg), " vi ", gViFrame, "\n");
  // Saved for the last two started tasks if either is dumped; the replay picks the one with the task's colour image.
  if(!shouldDump(gGfxTask) && !(gGfxTask > 0 && shouldDump(gGfxTask - 1))) return;
  if(width > 640) return;
  vector<u8> fb; fb.resize(width * 240 * 2 + 8);
  for(u32 i = 0; i < width * 240 * 2; i += 4) {
    u32 w = rdram.ram.read<Word>(cimg + i);
    fb[i] = w >> 24; fb[i+1] = w >> 16; fb[i+2] = w >> 8; fb[i+3] = w;
  }
  u32 hdr[2] = {cimg, width};
  memory::copy(fb.data() + width * 240 * 2, hdr, 8);
  file::write({gOutDir, "/sync_", pad(gGfxTask, 6, '0'), "_", pad(gSyncSeq, 7, '0'), ".fb"}, fb);
}

static void spHook() {
  using namespace ares::Nintendo64;
  // OSTask is at the end of DMEM (0xFC0); type at +0, data_ptr at +0x30.
  u32 type = rsp.dmem.read<Word>(0xFC0);
  if(type != 1) return;
  gGfxTask++;
  if(!shouldDump(gGfxTask)) return;
  u32 size = rdram.ram.size;
  vector<u8> be; be.resize(size + 64);
  for(u32 a = 0; a < size; a += 4) {
    u32 w = rdram.ram.read<Word>(a);
    be[a] = w >> 24; be[a+1] = w >> 16; be[a+2] = w >> 8; be[a+3] = w;
  }
  for(u32 a = 0; a < 64; a += 4) {
    u32 w = rsp.dmem.read<Word>(0xFC0 + a);
    be[size+a] = w >> 24; be[size+a+1] = w >> 16; be[size+a+2] = w >> 8; be[size+a+3] = w;
  }
  write_gz({gOutDir, "/task_", pad(gGfxTask, 6, '0'), ".rdram.gz"}, be.data(), be.size());
  print("dumped gfx task ", gGfxTask, " at vi frame ", gViFrame, "\n");
}

struct Headless : ares::Platform {
  auto attach(ares::Node::Object) -> void override {}
  auto pak(ares::Node::Object node) -> shared_pointer<vfs::directory> override {
    if(node->name() == "Nintendo 64") return gSystem->pak;
    if(node->name() == "Nintendo 64 Cartridge") return gGame->pak;
    return {};
  }
  auto status(string_view message) -> void override { print("status: ", message, "\n"); }
  auto video(ares::Node::Video::Screen, const u32* data, u32 pitch, u32 width, u32 height) -> void override {
    gViFrame++;
    if(getenv("VI_LOG")) print("vi ", gViFrame, " ", width, "x", height, "\n");
    if(gVideoEvery && gViFrame % gVideoEvery == 0) {
      // PPM, 24 bit
      string path = {gOutDir, "/vi_", pad(gViFrame, 6, '0'), ".ppm"};
      file_buffer fp{path, file::mode::write};
      fp.print("P6\n", width, " ", height, "\n255\n");
      for(u32 y : range(height)) for(u32 x : range(width)) {
        u32 c = data[y * (pitch >> 2) + x];
        fp.write(c >> 16 & 255); fp.write(c >> 8 & 255); fp.write(c & 255);
      }
    }
  }
  auto audio(ares::Node::Audio::Stream stream) -> void override {
    while(stream->pending()) { f64 b[2]; stream->read(b); }
  }
  auto input(ares::Node::Input::Input node) -> void override {
    auto name = node->name();
    if(auto axis = node->cast<ares::Node::Input::Axis>()) {
      if(name == "X-Axis") axis->setValue(gX * 32767 / 85);
      if(name == "Y-Axis") axis->setValue(-gY * 32767 / 85);
      return;
    }
    if(auto button = node->cast<ares::Node::Input::Button>()) {
      u32 bit = 0;
      if(name == "A") bit = B_A; if(name == "B") bit = B_B; if(name == "Z") bit = B_Z; if(name == "Start") bit = B_START;
      if(name == "Up") bit = B_UP; if(name == "Down") bit = B_DOWN; if(name == "Left") bit = B_LEFT; if(name == "Right") bit = B_RIGHT;
      if(name == "L") bit = B_L; if(name == "R") bit = B_R;
      if(name == "C-Up") bit = B_CU; if(name == "C-Down") bit = B_CD; if(name == "C-Left") bit = B_CL; if(name == "C-Right") bit = B_CR;
      button->setValue((gButtons & bit) != 0);
    }
  }
} headless;

// Script: "frame:BUTTON+BUTTON,STICK=x,y frame: ..." (frames are VI frames)
static void parseScript(string s) {
  for(auto& tok : s.split(" ")) {
    if(!tok) continue;
    auto parts = tok.split(":", 1L);
    Step st{(u32)parts[0].natural(), 0, 0, 0};
    if(parts.size() > 1) for(auto& item : parts[1].split("+")) {
      if(item.beginsWith("STICK=")) { auto xy = string{item}.trimLeft("STICK=", 1L).split(","); st.x = xy[0].integer(); st.y = xy[1].integer(); continue; }
      if(item == "A") st.buttons |= B_A; if(item == "B") st.buttons |= B_B; if(item == "Z") st.buttons |= B_Z;
      if(item == "START") st.buttons |= B_START; if(item == "UP") st.buttons |= B_UP; if(item == "DOWN") st.buttons |= B_DOWN;
      if(item == "LEFT") st.buttons |= B_LEFT; if(item == "RIGHT") st.buttons |= B_RIGHT; if(item == "L") st.buttons |= B_L; if(item == "R") st.buttons |= B_R;
      if(item == "CU") st.buttons |= B_CU; if(item == "CD") st.buttons |= B_CD; if(item == "CL") st.buttons |= B_CL; if(item == "CR") st.buttons |= B_CR;
    }
    gScript.append(st);
  }
}

auto nall::main(Arguments arguments) -> void {
  string rom, script; u32 frames = 600;
  arguments.take("--rom", rom);
  arguments.take("--out", gOutDir);
  arguments.take("--script", script);
  string v;
  if(arguments.take("--frames", v)) frames = v.natural();
  if(arguments.take("--video-every", v)) gVideoEvery = v.natural();
  if(arguments.take("--dump-tasks", v)) for(auto& t : v.split(",")) gDumpTasks.append(t.natural());
  if(arguments.take("--dump-every", v)) gDumpEvery = v.natural();
  if(arguments.take("--dump-from", v)) gDumpFrom = v.natural();
  if(arguments.take("--dump-to", v)) gDumpTo = v.natural();
  bool gpu = !arguments.take("--no-gpu");
  bool noVi = arguments.take("--no-vi");
  parseScript(script);
  directory::create(gOutDir);

  mia::setHomeLocation([]() -> string { return "/tmp/ares-home/"; });
  mia::setSaveLocation([]() -> string { return "/tmp/ares-saves/"; });
  ares::platform = &headless;
  ares::Nintendo64::spStartHook = spHook;
  ares::Nintendo64::rdpSyncHook = syncHook;

  gGame = mia::Medium::create("Nintendo 64");
  if(!gGame->load(rom)) { print("rom load failed\n"); return; }
  gSystem = mia::System::create("Nintendo 64");
  if(!gSystem->load()) { print("system load failed\n"); return; }
  ares::Nintendo64::option("Quality", "SD");
  ares::Nintendo64::option("Supersampling", false);
  ares::Nintendo64::option("Enable GPU acceleration", gpu);
  ares::Nintendo64::option("Disable Video Interface Processing", noVi);
  ares::Nintendo64::option("Weave Deinterlacing", true);
  ares::Nintendo64::option("Homebrew Mode", false);
  if(!ares::Nintendo64::load(gRoot, string{"[Nintendo] Nintendo 64 (", gGame->pak->attribute("region"), ")"})) { print("core load failed\n"); return; }
  if(auto port = gRoot->find<ares::Node::Port>("Cartridge Slot")) { port->allocate(); port->connect(); }
  if(auto port = gRoot->find<ares::Node::Port>("Controller Port 1")) { port->allocate("Gamepad"); port->connect(); }
  gRoot->power();
  u32 stepIndex = 0;
  while(gViFrame < frames) {
    while(stepIndex < gScript.size() && gScript[stepIndex].frame <= gViFrame) {
      gButtons = gScript[stepIndex].buttons; gX = gScript[stepIndex].x; gY = gScript[stepIndex].y; stepIndex++;
    }
    gRoot->run();
  }
  print("done: ", gViFrame, " vi frames, ", gGfxTask, " gfx tasks\n");
  gRoot->unload();
}
