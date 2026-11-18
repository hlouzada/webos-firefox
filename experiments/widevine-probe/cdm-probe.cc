// Loads the TV's Widevine wrapper the way Firefox's GMP process does and
// reports whether the CDM initializes. Prints every host callback.
//   cdm-probe [path-to-libwvcdm_shared.so]
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <vector>
#include "content_decryption_module.h"

static bool g_done = false;

struct Buf : cdm::Buffer {
  std::vector<uint8_t> d; uint32_t sz = 0;
  explicit Buf(uint32_t c) : d(c) {}
  void Destroy() override { delete this; }
  uint32_t Capacity() const override { return d.size(); }
  uint8_t* Data() override { return d.data(); }
  void SetSize(uint32_t s) override { sz = s; }
  uint32_t Size() const override { return sz; }
};

struct Host : cdm::Host_10 {
  cdm::Buffer* Allocate(uint32_t c) override { return new Buf(c); }
  void SetTimer(int64_t ms, void* ctx) override { printf("host: SetTimer(%lld)\n", (long long)ms); }
  cdm::Time GetCurrentWallTime() override { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec + tv.tv_usec / 1e6; }
  void OnInitialized(bool ok) override { printf("host: OnInitialized(%s)\n", ok ? "true" : "false"); g_done = true; }
  void OnResolveKeyStatusPromise(uint32_t, cdm::KeyStatus) override { puts("host: OnResolveKeyStatusPromise"); }
  void OnResolveNewSessionPromise(uint32_t, const char*, uint32_t) override { puts("host: OnResolveNewSessionPromise"); }
  void OnResolvePromise(uint32_t) override { puts("host: OnResolvePromise"); }
  void OnRejectPromise(uint32_t, cdm::Exception e, uint32_t code, const char* m, uint32_t n) override {
    printf("host: OnRejectPromise exception=%d code=%u msg=%.*s\n", (int)e, code, (int)n, m ? m : ""); }
  void OnSessionMessage(const char*, uint32_t, cdm::MessageType, const char*, uint32_t n) override { printf("host: OnSessionMessage %u bytes\n", n); }
  void OnSessionKeysChange(const char*, uint32_t, bool, const cdm::KeyInformation*, uint32_t) override { puts("host: OnSessionKeysChange"); }
  void OnExpirationChange(const char*, uint32_t, cdm::Time) override {}
  void OnSessionClosed(const char*, uint32_t) override { puts("host: OnSessionClosed"); }
  void SendPlatformChallenge(const char*, uint32_t, const char*, uint32_t) override { puts("host: SendPlatformChallenge"); }
  void EnableOutputProtection(uint32_t) override { puts("host: EnableOutputProtection"); }
  void QueryOutputProtectionStatus() override { puts("host: QueryOutputProtectionStatus"); }
  void OnDeferredInitializationDone(cdm::StreamType, cdm::Status) override {}
  cdm::FileIO* CreateFileIO(cdm::FileIOClient*) override { puts("host: CreateFileIO -> null"); return nullptr; }
  void RequestStorageId(uint32_t) override { puts("host: RequestStorageId"); }
};

static Host g_host;
static void* GetHost(int version, void*) { printf("cdm asks for host %d\n", version); return version == 10 ? static_cast<cdm::Host_10*>(&g_host) : nullptr; }

int main(int argc, char** argv) {
  const char* lib = argc > 1 ? argv[1] : "/usr/lib/libwvcdm_shared.so";
  void* h = dlopen(lib, RTLD_NOW);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  typedef void (*InitFn)(); typedef void* (*CreateFn)(int, const char*, uint32_t, void* (*)(int, void*), void*);
  typedef const char* (*VerFn)();
  InitFn init = (InitFn)dlsym(h, "InitializeCdmModule_10");
  CreateFn create = (CreateFn)dlsym(h, "CreateCdmInstance");
  VerFn ver = (VerFn)dlsym(h, "GetCdmVersion");
  if (!init || !create) { puts("symbols missing"); return 1; }
  if (ver) printf("GetCdmVersion: %s\n", ver());
  init(); puts("InitializeCdmModule_10 done");
  const char ks[] = "com.widevine.alpha";
  auto* cdm = static_cast<cdm::ContentDecryptionModule_10*>(create(10, ks, sizeof ks - 1, GetHost, nullptr));
  printf("CreateCdmInstance -> %p\n", (void*)cdm);
  if (!cdm) return 1;
  bool hw = argc > 2 ? atoi(argv[2]) != 0 : true;
  printf("Initialize(distinctive=false, persistent=true, hw_secure=%d)\n", hw);
  cdm->Initialize(false, true, hw);
  for (int i = 0; i < 100 && !g_done; i++) usleep(100000);
  if (!g_done) puts("no OnInitialized within 10 s");
  return g_done ? 0 : 2;
}
