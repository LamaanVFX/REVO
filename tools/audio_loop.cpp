#include <Windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
std::atomic<bool> running{true};

BOOL WINAPI ConsoleHandler(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
      event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
    running.store(false, std::memory_order_release);
    return TRUE;
  }
  return FALSE;
}
}  // namespace

int main(int argc, char** argv) {
  const auto frequency = argc > 1 ? std::atoi(argv[1]) : 440;
  if (frequency < 100 || frequency > 2000) return 9;

  WAVEFORMATEX format{};
  format.wFormatTag = WAVE_FORMAT_PCM;
  format.nChannels = 2;
  format.nSamplesPerSec = 48000;
  format.wBitsPerSample = 16;
  format.nBlockAlign = 4;
  format.nAvgBytesPerSec = 192000;

  HWAVEOUT device = nullptr;
  const auto opened = waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
  if (opened != MMSYSERR_NOERROR) {
    std::fprintf(stderr, "waveOutOpen failed: %u\n", static_cast<unsigned>(opened));
    return 2;
  }

  constexpr int kFrames = 4800;  // 100 ms, an integer number of cycles for test tones.
  std::vector<short> samples(static_cast<std::size_t>(kFrames) * 2);
  for (int frame = 0; frame < kFrames; ++frame) {
    const auto sample = static_cast<short>(
        4096.0 * std::sin(6.283185307179586 * frequency * frame / 48000.0));
    samples[static_cast<std::size_t>(frame) * 2] = sample;
    samples[static_cast<std::size_t>(frame) * 2 + 1] = sample;
  }

  WAVEHDR header{};
  header.lpData = reinterpret_cast<char*>(samples.data());
  header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(short));
  if (waveOutPrepareHeader(device, &header, sizeof(header)) != MMSYSERR_NOERROR) {
    waveOutClose(device);
    return 3;
  }
  SetConsoleCtrlHandler(ConsoleHandler, TRUE);

  while (running.load(std::memory_order_acquire)) {
    if ((header.dwFlags & WHDR_INQUEUE) == 0 &&
        waveOutWrite(device, &header, sizeof(header)) != MMSYSERR_NOERROR) {
      running.store(false, std::memory_order_release);
      break;
    }
    Sleep(1);
  }

  waveOutReset(device);
  waveOutUnprepareHeader(device, &header, sizeof(header));
  waveOutClose(device);
  return 0;
}
