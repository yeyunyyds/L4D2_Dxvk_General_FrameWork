// Exercise the prepared production full IPC adapter, including its real ring,
// shared mapping, wake events and consumer credits. No GPU or Source calls.
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
namespace bridge_util { enum class Result { Success, Timeout, Failure }; }
#ifdef _MSC_VER
// Pinned headers deliberately pad cache lines and assign while rechecking wait
// predicates. ABI assertions and the behavior below validate those choices.
// Keep /W4 /WX for all other diagnostics and for this fixture's own code.
#pragma warning(push)
#pragma warning(disable: 4324 4706)
#endif
#include "ipc_full/message_queue.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

std::atomic<bool> gbBridgeRunning{true};
using Queue = ldb_full::MessageQueue;
void require(bool ok, const char* why) { if (!ok) { throw std::runtime_error(why); } }
std::string name(const char* suffix) {
  return "rt-transport-" + std::to_string(ldb_ipc_v2::processId()) + "-" + suffix;
}
constexpr size_t poolBytes = ldb_full::PoolMetadata + 4 * ldb_full::PageBytes;
bool send(Queue& writer, uint32_t ordinal) {
  return writer.begin({Commands::IDirect3DDevice9Ex_SetStreamSource, 0, 0, 2}) &&
    writer.scalar(ordinal) && writer.scalar(0) && writer.scalar(42) && writer.scalar(0) && writer.scalar(48) &&
    writer.finish(true);
}
void fill(Queue& writer) {
  for (uint32_t i = 0; i < 64; ++i) { require(send(writer, i), "fill ring without dropping commands"); }
}
void receive(Queue& reader, uint32_t ordinal) {
  bridge_util::Result result;
  const auto h = reader.peek(result, 1000);
  require(result == bridge_util::Result::Success && h.command == Commands::IDirect3DDevice9Ex_SetStreamSource &&
    h.pHandle == 2, "command and object identity");
  require(reader.readWord() == ordinal && reader.readWord() == 0 && reader.readWord() == 42 &&
    reader.readWord() == 0 && reader.readWord() == 48, "FIFO and complete stream-source payload");
  reader.finishRead();
}
int main() {
  try {
    using namespace ldb_rt::transport_wait;
    configure(false); require(producerMs() == 2000, "normal profile budget unchanged");
    {
      Queue writer(name("normal"), poolBytes, true), reader(name("normal"), poolBytes, false);
      fill(writer);
      require(!send(writer, 64) && writer.transportFault() == uint32_t(ldb_ipc_v2::Fault::Timeout),
        "reproduce default full-ring producer timeout");
    }
    configure(true); require(producerMs() == 60000, "world profile finite compilation budget");
    {
      Queue writer(name("world"), poolBytes, true), reader(name("world"), poolBytes, false);
      fill(writer);
      std::exception_ptr failure;
      std::thread consumer([&] {
        try {
          // This exceeds the production default 2 s wait, without waiting for
          // the extended bound. A late consumer must retain every command.
          std::this_thread::sleep_for(std::chrono::milliseconds(2300));
          for (uint32_t i = 0; i < 65; ++i) { receive(reader, i); }
        } catch (...) { failure = std::current_exception(); }
      });
      const bool accepted = send(writer, 64);
      consumer.join(); if (failure) { std::rethrow_exception(failure); }
      require(accepted && writer.healthy() && writer.transportFault() == 0 && reader.isEmpty(),
        "world profile survives >2 s pause with ordered delivery");
    }
    {
      Queue writer(name("closed"), poolBytes, true);
      auto reader = std::make_unique<Queue>(name("closed"), poolBytes, false);
      fill(writer);
      std::thread closer([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); reader.reset(); });
      const auto start = ldb_ipc_v2::milliseconds();
      const bool accepted = send(writer, 64);
      closer.join();
      require(!accepted && writer.transportFault() == uint32_t(ldb_ipc_v2::Fault::PeerExited) &&
        ldb_ipc_v2::milliseconds() - start < 2000, "peer close interrupts extended wait promptly");
    }
    configure(false); require(producerMs() == 2000, "normal profile restored");
    std::cout << "RT full IPC normal-timeout/world-delayed-consumer/FIFO/peer-close: PASS\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
