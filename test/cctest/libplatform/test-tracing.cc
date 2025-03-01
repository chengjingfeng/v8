// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <limits>

#include "include/libplatform/v8-tracing.h"
#include "src/base/platform/platform.h"
#include "src/libplatform/default-platform.h"
#include "src/tracing/trace-event.h"
#include "test/cctest/cctest.h"

#ifdef V8_USE_PERFETTO
#include "perfetto/trace/chrome/chrome_trace_event.pb.h"
#include "perfetto/trace/chrome/chrome_trace_event.pbzero.h"
#include "perfetto/trace/chrome/chrome_trace_packet.pb.h"
#include "perfetto/trace/trace.pb.h"
#include "perfetto/tracing.h"
#include "src/libplatform/tracing/json-trace-event-listener.h"
#include "src/libplatform/tracing/trace-event-listener.h"
#endif  // V8_USE_PERFETTO

#ifdef V8_USE_PERFETTO
class TestDataSource : public perfetto::DataSource<TestDataSource> {
 public:
  void OnSetup(const SetupArgs&) override {}
  void OnStart(const StartArgs&) override { started_.Signal(); }
  void OnStop(const StopArgs&) override {}

  static v8::base::Semaphore started_;
};

v8::base::Semaphore TestDataSource::started_{0};

PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(TestDataSource);
#endif  // V8_USE_PERFETTO

namespace v8 {
namespace platform {
namespace tracing {

TEST(TestTraceConfig) {
  LocalContext env;
  TraceConfig* trace_config = new TraceConfig();
  trace_config->AddIncludedCategory("v8");
  trace_config->AddIncludedCategory(TRACE_DISABLED_BY_DEFAULT("v8.runtime"));

  CHECK_EQ(trace_config->IsSystraceEnabled(), false);
  CHECK_EQ(trace_config->IsArgumentFilterEnabled(), false);
  CHECK_EQ(trace_config->IsCategoryGroupEnabled("v8"), true);
  CHECK_EQ(trace_config->IsCategoryGroupEnabled("v8.cpu_profile"), false);
  CHECK_EQ(trace_config->IsCategoryGroupEnabled("v8.cpu_profile.hires"), false);
  CHECK_EQ(trace_config->IsCategoryGroupEnabled(
               TRACE_DISABLED_BY_DEFAULT("v8.runtime")),
           true);
  CHECK_EQ(trace_config->IsCategoryGroupEnabled("v8,v8.cpu_profile"), true);
  CHECK_EQ(
      trace_config->IsCategoryGroupEnabled("v8,disabled-by-default-v8.runtime"),
      true);
  CHECK_EQ(trace_config->IsCategoryGroupEnabled(
               "v8_cpu_profile,v8.cpu_profile.hires"),
           false);

  delete trace_config;
}

TEST(TestTraceObject) {
  TraceObject trace_object;
  uint8_t category_enabled_flag = 41;
  trace_object.Initialize('X', &category_enabled_flag, "Test.Trace",
                          "Test.Scope", 42, 123, 0, nullptr, nullptr, nullptr,
                          nullptr, 0, 1729, 4104);
  CHECK_EQ('X', trace_object.phase());
  CHECK_EQ(category_enabled_flag, *trace_object.category_enabled_flag());
  CHECK_EQ(std::string("Test.Trace"), std::string(trace_object.name()));
  CHECK_EQ(std::string("Test.Scope"), std::string(trace_object.scope()));
  CHECK_EQ(0u, trace_object.duration());
  CHECK_EQ(0u, trace_object.cpu_duration());
}

class ConvertableToTraceFormatMock : public v8::ConvertableToTraceFormat {
 public:
  explicit ConvertableToTraceFormatMock(int value) : value_(value) {}
  void AppendAsTraceFormat(std::string* out) const override {
    *out += "[" + std::to_string(value_) + "," + std::to_string(value_) + "]";
  }

 private:
  int value_;

  DISALLOW_COPY_AND_ASSIGN(ConvertableToTraceFormatMock);
};

class MockTraceWriter : public TraceWriter {
 public:
  void AppendTraceEvent(TraceObject* trace_event) override {
    // TraceObject might not have been initialized.
    const char* name = trace_event->name() ? trace_event->name() : "";
    events_.push_back(name);
  }

  void Flush() override {}

  std::vector<std::string> events() { return events_; }

 private:
  std::vector<std::string> events_;
};

TEST(TestTraceBufferRingBuffer) {
  // We should be able to add kChunkSize * 2 + 1 trace events.
  const int HANDLES_COUNT = TraceBufferChunk::kChunkSize * 2 + 1;
  MockTraceWriter* writer = new MockTraceWriter();
  TraceBuffer* ring_buffer =
      TraceBuffer::CreateTraceBufferRingBuffer(2, writer);
  std::string names[HANDLES_COUNT];
  for (int i = 0; i < HANDLES_COUNT; ++i) {
    names[i] = "Test.EventNo" + std::to_string(i);
  }

  std::vector<uint64_t> handles(HANDLES_COUNT);
  uint8_t category_enabled_flag = 41;
  for (size_t i = 0; i < handles.size(); ++i) {
    TraceObject* trace_object = ring_buffer->AddTraceEvent(&handles[i]);
    CHECK_NOT_NULL(trace_object);
    trace_object->Initialize('X', &category_enabled_flag, names[i].c_str(),
                             "Test.Scope", 42, 123, 0, nullptr, nullptr,
                             nullptr, nullptr, 0, 1729, 4104);
    trace_object = ring_buffer->GetEventByHandle(handles[i]);
    CHECK_NOT_NULL(trace_object);
    CHECK_EQ('X', trace_object->phase());
    CHECK_EQ(names[i], std::string(trace_object->name()));
    CHECK_EQ(category_enabled_flag, *trace_object->category_enabled_flag());
  }

  // We should only be able to retrieve the last kChunkSize + 1.
  for (size_t i = 0; i < TraceBufferChunk::kChunkSize; ++i) {
    CHECK_NULL(ring_buffer->GetEventByHandle(handles[i]));
  }

  for (size_t i = TraceBufferChunk::kChunkSize; i < handles.size(); ++i) {
    TraceObject* trace_object = ring_buffer->GetEventByHandle(handles[i]);
    CHECK_NOT_NULL(trace_object);
    // The object properties should be correct.
    CHECK_EQ('X', trace_object->phase());
    CHECK_EQ(names[i], std::string(trace_object->name()));
    CHECK_EQ(category_enabled_flag, *trace_object->category_enabled_flag());
  }

  // Check Flush(), that the writer wrote the last kChunkSize  1 event names.
  ring_buffer->Flush();
  auto events = writer->events();
  CHECK_EQ(TraceBufferChunk::kChunkSize + 1, events.size());
  for (size_t i = TraceBufferChunk::kChunkSize; i < handles.size(); ++i) {
    CHECK_EQ(names[i], events[i - TraceBufferChunk::kChunkSize]);
  }
  delete ring_buffer;
}

void PopulateJSONWriter(TraceWriter* writer) {
  v8::Platform* old_platform = i::V8::GetCurrentPlatform();
  std::unique_ptr<v8::Platform> default_platform(
      v8::platform::NewDefaultPlatform());
  i::V8::SetPlatformForTesting(default_platform.get());
  auto tracing = base::make_unique<v8::platform::tracing::TracingController>();
  v8::platform::tracing::TracingController* tracing_controller = tracing.get();
  static_cast<v8::platform::DefaultPlatform*>(default_platform.get())
      ->SetTracingController(std::move(tracing));

  TraceBuffer* ring_buffer =
      TraceBuffer::CreateTraceBufferRingBuffer(1, writer);
  tracing_controller->Initialize(ring_buffer);
#ifdef V8_USE_PERFETTO
  std::ostringstream sstream;
  tracing_controller->InitializeForPerfetto(&sstream);
#endif
  TraceConfig* trace_config = new TraceConfig();
  trace_config->AddIncludedCategory("v8-cat");
  tracing_controller->StartTracing(trace_config);

  TraceObject trace_object;
  trace_object.InitializeForTesting(
      'X', tracing_controller->GetCategoryGroupEnabled("v8-cat"), "Test0",
      v8::internal::tracing::kGlobalScope, 42, 0x1234, 0, nullptr, nullptr,
      nullptr, nullptr, TRACE_EVENT_FLAG_HAS_ID, 11, 22, 100, 50, 33, 44);
  writer->AppendTraceEvent(&trace_object);
  trace_object.InitializeForTesting(
      'Y', tracing_controller->GetCategoryGroupEnabled("v8-cat"), "Test1",
      v8::internal::tracing::kGlobalScope, 43, 0x5678, 0, nullptr, nullptr,
      nullptr, nullptr, TRACE_EVENT_FLAG_FLOW_IN | TRACE_EVENT_FLAG_FLOW_OUT,
      55, 66, 110, 55, 77, 88);
  writer->AppendTraceEvent(&trace_object);
  tracing_controller->StopTracing();
  i::V8::SetPlatformForTesting(old_platform);
}

TEST(TestJSONTraceWriter) {
  std::ostringstream stream;
  TraceWriter* writer = TraceWriter::CreateJSONTraceWriter(stream);
  PopulateJSONWriter(writer);
  std::string trace_str = stream.str();
  std::string expected_trace_str =
      "{\"traceEvents\":[{\"pid\":11,\"tid\":22,\"ts\":100,\"tts\":50,"
      "\"ph\":\"X\",\"cat\":\"v8-cat\",\"name\":\"Test0\",\"dur\":33,"
      "\"tdur\":44,\"id\":\"0x2a\",\"args\":{}},{\"pid\":55,\"tid\":66,"
      "\"ts\":110,\"tts\":55,\"ph\":\"Y\",\"cat\":\"v8-cat\",\"name\":"
      "\"Test1\",\"dur\":77,\"tdur\":88,\"bind_id\":\"0x5678\","
      "\"flow_in\":true,\"flow_out\":true,\"args\":{}}]}";

  CHECK_EQ(expected_trace_str, trace_str);
}

TEST(TestJSONTraceWriterWithCustomtag) {
  std::ostringstream stream;
  TraceWriter* writer = TraceWriter::CreateJSONTraceWriter(stream, "customTag");
  PopulateJSONWriter(writer);
  std::string trace_str = stream.str();
  std::string expected_trace_str =
      "{\"customTag\":[{\"pid\":11,\"tid\":22,\"ts\":100,\"tts\":50,"
      "\"ph\":\"X\",\"cat\":\"v8-cat\",\"name\":\"Test0\",\"dur\":33,"
      "\"tdur\":44,\"id\":\"0x2a\",\"args\":{}},{\"pid\":55,\"tid\":66,"
      "\"ts\":110,\"tts\":55,\"ph\":\"Y\",\"cat\":\"v8-cat\",\"name\":"
      "\"Test1\",\"dur\":77,\"tdur\":88,\"bind_id\":\"0x5678\","
      "\"flow_in\":true,\"flow_out\":true,\"args\":{}}]}";

  CHECK_EQ(expected_trace_str, trace_str);
}

TEST(TestTracingController) {
  v8::Platform* old_platform = i::V8::GetCurrentPlatform();
  std::unique_ptr<v8::Platform> default_platform(
      v8::platform::NewDefaultPlatform());
  i::V8::SetPlatformForTesting(default_platform.get());

  auto tracing = base::make_unique<v8::platform::tracing::TracingController>();
  v8::platform::tracing::TracingController* tracing_controller = tracing.get();
  static_cast<v8::platform::DefaultPlatform*>(default_platform.get())
      ->SetTracingController(std::move(tracing));

  MockTraceWriter* writer = new MockTraceWriter();
  TraceBuffer* ring_buffer =
      TraceBuffer::CreateTraceBufferRingBuffer(1, writer);
  tracing_controller->Initialize(ring_buffer);
#ifdef V8_USE_PERFETTO
  std::ostringstream sstream;
  tracing_controller->InitializeForPerfetto(&sstream);
#endif
  TraceConfig* trace_config = new TraceConfig();
  trace_config->AddIncludedCategory("v8");
  tracing_controller->StartTracing(trace_config);

  TRACE_EVENT0("v8", "v8.Test");
  // cat category is not included in default config
  TRACE_EVENT0("cat", "v8.Test2");
  TRACE_EVENT0("v8", "v8.Test3");
  tracing_controller->StopTracing();

  CHECK_EQ(2u, writer->events().size());
  CHECK_EQ(std::string("v8.Test"), writer->events()[0]);
  CHECK_EQ(std::string("v8.Test3"), writer->events()[1]);

  i::V8::SetPlatformForTesting(old_platform);
}

void GetJSONStrings(
    std::vector<std::string>& ret,  // NOLINT(runtime/references)
    std::string str, std::string param, std::string start_delim,
    std::string end_delim) {
  size_t pos = str.find(param);
  while (pos != std::string::npos) {
    size_t start_pos = str.find(start_delim, pos + param.length());
    size_t end_pos = str.find(end_delim, start_pos + 1);
    CHECK_NE(start_pos, std::string::npos);
    CHECK_NE(end_pos, std::string::npos);
    ret.push_back(str.substr(start_pos + 1, end_pos - start_pos - 1));
    pos = str.find(param, pos + 1);
  }
}

TEST(TestTracingControllerMultipleArgsAndCopy) {
  std::ostringstream stream, perfetto_stream;
  uint64_t aa = 11;
  unsigned int bb = 22;
  uint16_t cc = 33;
  unsigned char dd = 44;
  int64_t ee = -55;
  int ff = -66;
  int16_t gg = -77;
  signed char hh = -88;
  bool ii1 = true;
  bool ii2 = false;
  double jj1 = 99.0;
  double jj2 = 1e100;
  double jj3 = std::numeric_limits<double>::quiet_NaN();
  double jj4 = std::numeric_limits<double>::infinity();
  double jj5 = -std::numeric_limits<double>::infinity();
  void* kk = &aa;
  const char* ll = "100";
  std::string mm = "INIT";
  std::string mmm = "\"INIT\"";

  // Create a scope for the tracing controller to terminate the trace writer.
  {
    v8::Platform* old_platform = i::V8::GetCurrentPlatform();
    std::unique_ptr<v8::Platform> default_platform(
        v8::platform::NewDefaultPlatform());
    i::V8::SetPlatformForTesting(default_platform.get());

    auto tracing =
        base::make_unique<v8::platform::tracing::TracingController>();
    v8::platform::tracing::TracingController* tracing_controller =
        tracing.get();
    static_cast<v8::platform::DefaultPlatform*>(default_platform.get())
        ->SetTracingController(std::move(tracing));
    TraceWriter* writer = TraceWriter::CreateJSONTraceWriter(stream);

    TraceBuffer* ring_buffer =
        TraceBuffer::CreateTraceBufferRingBuffer(1, writer);
    tracing_controller->Initialize(ring_buffer);
#ifdef V8_USE_PERFETTO
    tracing_controller->InitializeForPerfetto(&perfetto_stream);
#endif
    TraceConfig* trace_config = new TraceConfig();
    trace_config->AddIncludedCategory("v8");
    tracing_controller->StartTracing(trace_config);

    {
      TRACE_EVENT1("v8", "v8.Test.aa", "aa", aa);
      TRACE_EVENT1("v8", "v8.Test.bb", "bb", bb);
      TRACE_EVENT1("v8", "v8.Test.cc", "cc", cc);
      TRACE_EVENT1("v8", "v8.Test.dd", "dd", dd);
      TRACE_EVENT1("v8", "v8.Test.ee", "ee", ee);
      TRACE_EVENT1("v8", "v8.Test.ff", "ff", ff);
      TRACE_EVENT1("v8", "v8.Test.gg", "gg", gg);
      TRACE_EVENT1("v8", "v8.Test.hh", "hh", hh);
      TRACE_EVENT1("v8", "v8.Test.ii", "ii1", ii1);
      TRACE_EVENT1("v8", "v8.Test.ii", "ii2", ii2);
      TRACE_EVENT1("v8", "v8.Test.jj1", "jj1", jj1);
      TRACE_EVENT1("v8", "v8.Test.jj2", "jj2", jj2);
      TRACE_EVENT1("v8", "v8.Test.jj3", "jj3", jj3);
      TRACE_EVENT1("v8", "v8.Test.jj4", "jj4", jj4);
      TRACE_EVENT1("v8", "v8.Test.jj5", "jj5", jj5);
      TRACE_EVENT1("v8", "v8.Test.kk", "kk", kk);
      TRACE_EVENT1("v8", "v8.Test.ll", "ll", ll);
      TRACE_EVENT1("v8", "v8.Test.mm", "mm", TRACE_STR_COPY(mmm.c_str()));

      TRACE_EVENT2("v8", "v8.Test2.1", "aa", aa, "ll", ll);
      TRACE_EVENT2("v8", "v8.Test2.2", "mm1", TRACE_STR_COPY(mm.c_str()), "mm2",
                   TRACE_STR_COPY(mmm.c_str()));

      // Check copies are correct.
      TRACE_EVENT_COPY_INSTANT0("v8", mm.c_str(), TRACE_EVENT_SCOPE_THREAD);
      TRACE_EVENT_COPY_INSTANT2("v8", mm.c_str(), TRACE_EVENT_SCOPE_THREAD,
                                "mm1", mm.c_str(), "mm2", mmm.c_str());
      mm = "CHANGED";
      mmm = "CHANGED";

      TRACE_EVENT_INSTANT1("v8", "v8.Test", TRACE_EVENT_SCOPE_THREAD, "a1",
                           new ConvertableToTraceFormatMock(42));
      std::unique_ptr<ConvertableToTraceFormatMock> trace_event_arg(
          new ConvertableToTraceFormatMock(42));
      TRACE_EVENT_INSTANT2("v8", "v8.Test", TRACE_EVENT_SCOPE_THREAD, "a1",
                           std::move(trace_event_arg), "a2",
                           new ConvertableToTraceFormatMock(123));
    }

    tracing_controller->StopTracing();

    i::V8::SetPlatformForTesting(old_platform);
  }

  std::string trace_str = stream.str();

  std::vector<std::string> all_args, all_names, all_cats;
  GetJSONStrings(all_args, trace_str, "\"args\"", "{", "}");
  GetJSONStrings(all_names, trace_str, "\"name\"", "\"", "\"");
  GetJSONStrings(all_cats, trace_str, "\"cat\"", "\"", "\"");

  CHECK_EQ(all_args.size(), 24u);
  CHECK_EQ(all_args[0], "\"aa\":11");
  CHECK_EQ(all_args[1], "\"bb\":22");
  CHECK_EQ(all_args[2], "\"cc\":33");
  CHECK_EQ(all_args[3], "\"dd\":44");
  CHECK_EQ(all_args[4], "\"ee\":-55");
  CHECK_EQ(all_args[5], "\"ff\":-66");
  CHECK_EQ(all_args[6], "\"gg\":-77");
  CHECK_EQ(all_args[7], "\"hh\":-88");
  CHECK_EQ(all_args[8], "\"ii1\":true");
  CHECK_EQ(all_args[9], "\"ii2\":false");
  CHECK_EQ(all_args[10], "\"jj1\":99.0");
  CHECK_EQ(all_args[11], "\"jj2\":1e+100");
  CHECK_EQ(all_args[12], "\"jj3\":\"NaN\"");
  CHECK_EQ(all_args[13], "\"jj4\":\"Infinity\"");
  CHECK_EQ(all_args[14], "\"jj5\":\"-Infinity\"");
  std::ostringstream pointer_stream;
  pointer_stream << "\"kk\":\"" << &aa << "\"";
  CHECK_EQ(all_args[15], pointer_stream.str());
  CHECK_EQ(all_args[16], "\"ll\":\"100\"");
  CHECK_EQ(all_args[17], "\"mm\":\"\\\"INIT\\\"\"");

  CHECK_EQ(all_names[18], "v8.Test2.1");
  CHECK_EQ(all_args[18], "\"aa\":11,\"ll\":\"100\"");
  CHECK_EQ(all_args[19], "\"mm1\":\"INIT\",\"mm2\":\"\\\"INIT\\\"\"");

  CHECK_EQ(all_names[20], "INIT");
  CHECK_EQ(all_names[21], "INIT");
  CHECK_EQ(all_args[21], "\"mm1\":\"INIT\",\"mm2\":\"\\\"INIT\\\"\"");
  CHECK_EQ(all_args[22], "\"a1\":[42,42]");
  CHECK_EQ(all_args[23], "\"a1\":[42,42],\"a2\":[123,123]");
}

namespace {

class TraceStateObserverImpl : public TracingController::TraceStateObserver {
 public:
  void OnTraceEnabled() override { ++enabled_count; }
  void OnTraceDisabled() override { ++disabled_count; }

  int enabled_count = 0;
  int disabled_count = 0;
};

}  // namespace

TEST(TracingObservers) {
  v8::Platform* old_platform = i::V8::GetCurrentPlatform();
  std::unique_ptr<v8::Platform> default_platform(
      v8::platform::NewDefaultPlatform());
  i::V8::SetPlatformForTesting(default_platform.get());

  auto tracing = base::make_unique<v8::platform::tracing::TracingController>();
  v8::platform::tracing::TracingController* tracing_controller = tracing.get();
  static_cast<v8::platform::DefaultPlatform*>(default_platform.get())
      ->SetTracingController(std::move(tracing));
  MockTraceWriter* writer = new MockTraceWriter();
  v8::platform::tracing::TraceBuffer* ring_buffer =
      v8::platform::tracing::TraceBuffer::CreateTraceBufferRingBuffer(1,
                                                                      writer);
  tracing_controller->Initialize(ring_buffer);
#ifdef V8_USE_PERFETTO
  std::ostringstream sstream;
  tracing_controller->InitializeForPerfetto(&sstream);
#endif
  v8::platform::tracing::TraceConfig* trace_config =
      new v8::platform::tracing::TraceConfig();
  trace_config->AddIncludedCategory("v8");

  TraceStateObserverImpl observer;
  tracing_controller->AddTraceStateObserver(&observer);

  CHECK_EQ(0, observer.enabled_count);
  CHECK_EQ(0, observer.disabled_count);

  tracing_controller->StartTracing(trace_config);

  CHECK_EQ(1, observer.enabled_count);
  CHECK_EQ(0, observer.disabled_count);

  TraceStateObserverImpl observer2;
  tracing_controller->AddTraceStateObserver(&observer2);

  CHECK_EQ(1, observer2.enabled_count);
  CHECK_EQ(0, observer2.disabled_count);

  tracing_controller->RemoveTraceStateObserver(&observer2);

  CHECK_EQ(1, observer2.enabled_count);
  CHECK_EQ(0, observer2.disabled_count);

  tracing_controller->StopTracing();

  CHECK_EQ(1, observer.enabled_count);
  CHECK_EQ(1, observer.disabled_count);
  CHECK_EQ(1, observer2.enabled_count);
  CHECK_EQ(0, observer2.disabled_count);

  tracing_controller->RemoveTraceStateObserver(&observer);

  CHECK_EQ(1, observer.enabled_count);
  CHECK_EQ(1, observer.disabled_count);

  trace_config = new v8::platform::tracing::TraceConfig();
  tracing_controller->StartTracing(trace_config);
  tracing_controller->StopTracing();

  CHECK_EQ(1, observer.enabled_count);
  CHECK_EQ(1, observer.disabled_count);

  i::V8::SetPlatformForTesting(old_platform);
}

class TraceWritingThread : public base::Thread {
 public:
  TraceWritingThread(
      v8::platform::tracing::TracingController* tracing_controller)
      : base::Thread(base::Thread::Options("TraceWritingThread")),
        tracing_controller_(tracing_controller) {}

  void Run() override {
    running_.store(true);
    while (running_.load()) {
      TRACE_EVENT0("v8", "v8.Test");
      tracing_controller_->AddTraceEvent('A', nullptr, "v8", "", 1, 1, 0,
                                         nullptr, nullptr, nullptr, nullptr, 0);
      tracing_controller_->AddTraceEventWithTimestamp('A', nullptr, "v8", "", 1,
                                                      1, 0, nullptr, nullptr,
                                                      nullptr, nullptr, 0, 0);
    }
  }

  void Stop() { running_.store(false); }

 private:
  std::atomic_bool running_{false};
  v8::platform::tracing::TracingController* tracing_controller_;
};

TEST(AddTraceEventMultiThreaded) {
  v8::Platform* old_platform = i::V8::GetCurrentPlatform();
  std::unique_ptr<v8::Platform> default_platform(
      v8::platform::NewDefaultPlatform());
  i::V8::SetPlatformForTesting(default_platform.get());

  auto tracing = base::make_unique<v8::platform::tracing::TracingController>();
  v8::platform::tracing::TracingController* tracing_controller = tracing.get();
  static_cast<v8::platform::DefaultPlatform*>(default_platform.get())
      ->SetTracingController(std::move(tracing));

  MockTraceWriter* writer = new MockTraceWriter();
  TraceBuffer* ring_buffer =
      TraceBuffer::CreateTraceBufferRingBuffer(1, writer);
  tracing_controller->Initialize(ring_buffer);
#ifdef V8_USE_PERFETTO
  std::ostringstream sstream;
  tracing_controller->InitializeForPerfetto(&sstream);
#endif
  TraceConfig* trace_config = new TraceConfig();
  trace_config->AddIncludedCategory("v8");
  tracing_controller->StartTracing(trace_config);

  TraceWritingThread thread(tracing_controller);
  thread.StartSynchronously();
  TRACE_EVENT0("v8", "v8.Test2");
  TRACE_EVENT0("v8", "v8.Test2");

  base::OS::Sleep(base::TimeDelta::FromMilliseconds(10));
  tracing_controller->StopTracing();

  thread.Stop();
  thread.Join();

  i::V8::SetPlatformForTesting(old_platform);
}

#ifdef V8_USE_PERFETTO

struct TraceEvent {
  std::string name;
  int64_t timestamp;
  int32_t phase;
  int32_t thread_id;
  int64_t duration;
  int64_t thread_duration;
  std::string scope;
  uint64_t id;
  uint32_t flags;
  std::string category_group_name;
  int32_t process_id;
  int64_t thread_timestamp;
  uint64_t bind_id;
};

class TestListener : public TraceEventListener {
 public:
  void ProcessPacket(const ::perfetto::protos::TracePacket& packet) {
    for (const ::perfetto::protos::ChromeTraceEvent& event :
         packet.chrome_events().trace_events()) {
      TraceEvent trace_event{event.name(),       event.timestamp(),
                             event.phase(),      event.thread_id(),
                             event.duration(),   event.thread_duration(),
                             event.scope(),      event.id(),
                             event.flags(),      event.category_group_name(),
                             event.process_id(), event.thread_timestamp(),
                             event.bind_id()};
      events_.push_back(trace_event);
    }
  }

  TraceEvent* get_event(size_t index) { return &events_.at(index); }

  size_t events_size() const { return events_.size(); }

 private:
  std::vector<TraceEvent> events_;
};

class TracingTestHarness {
 public:
  TracingTestHarness() {
    old_platform_ = i::V8::GetCurrentPlatform();
    default_platform_ = v8::platform::NewDefaultPlatform();
    i::V8::SetPlatformForTesting(default_platform_.get());

    auto tracing =
        base::make_unique<v8::platform::tracing::TracingController>();
    tracing_controller_ = tracing.get();
    static_cast<v8::platform::DefaultPlatform*>(default_platform_.get())
        ->SetTracingController(std::move(tracing));

    MockTraceWriter* writer = new MockTraceWriter();
    TraceBuffer* ring_buffer =
        TraceBuffer::CreateTraceBufferRingBuffer(1, writer);
    tracing_controller_->Initialize(ring_buffer);
    tracing_controller_->InitializeForPerfetto(&perfetto_json_stream_);
    tracing_controller_->SetTraceEventListenerForTesting(&listener_);
  }

  ~TracingTestHarness() { i::V8::SetPlatformForTesting(old_platform_); }

  void StartTracing() {
    TraceConfig* trace_config = new TraceConfig();
    trace_config->AddIncludedCategory("v8");
    tracing_controller_->StartTracing(trace_config);
  }

  void StopTracing() { tracing_controller_->StopTracing(); }

  TraceEvent* get_event(size_t index) { return listener_.get_event(index); }
  size_t events_size() const { return listener_.events_size(); }

  std::string perfetto_json_stream() { return perfetto_json_stream_.str(); }

 private:
  std::unique_ptr<v8::Platform> default_platform_;
  v8::Platform* old_platform_;
  v8::platform::tracing::TracingController* tracing_controller_;
  TestListener listener_;
  std::ostringstream perfetto_json_stream_;
};

TEST(Perfetto) {
  TracingTestHarness harness;
  harness.StartTracing();

  uint64_t uint64_arg = 1024;
  const char* str_arg = "str_arg";

  {
    TRACE_EVENT0("v8", "test1");
    TRACE_EVENT1("v8", "test2", "arg1", uint64_arg);
    TRACE_EVENT2("v8", "test3", "arg1", uint64_arg, "arg2", str_arg);
  }
  TRACE_EVENT_INSTANT0("v8", "final event not captured",
                       TRACE_EVENT_SCOPE_THREAD);

  harness.StopTracing();

  TraceEvent* event = harness.get_event(0);
  int32_t thread_id = event->thread_id;
  int32_t process_id = event->process_id;
  CHECK_EQ("test1", event->name);
  CHECK_EQ(TRACE_EVENT_PHASE_BEGIN, event->phase);
  int64_t timestamp = event->timestamp;

  event = harness.get_event(1);
  CHECK_EQ("test2", event->name);
  CHECK_EQ(TRACE_EVENT_PHASE_BEGIN, event->phase);
  CHECK_EQ(thread_id, event->thread_id);
  CHECK_EQ(process_id, event->process_id);
  CHECK_GE(event->timestamp, timestamp);
  timestamp = event->timestamp;

  event = harness.get_event(2);
  CHECK_EQ("test3", event->name);
  CHECK_EQ(TRACE_EVENT_PHASE_BEGIN, event->phase);
  CHECK_EQ(thread_id, event->thread_id);
  CHECK_EQ(process_id, event->process_id);
  CHECK_GE(event->timestamp, timestamp);
  timestamp = event->timestamp;

  event = harness.get_event(3);
  CHECK_EQ(TRACE_EVENT_PHASE_END, event->phase);
  CHECK_EQ(thread_id, event->thread_id);
  CHECK_EQ(process_id, event->process_id);
  CHECK_GE(event->timestamp, timestamp);
  timestamp = event->timestamp;

  event = harness.get_event(4);
  CHECK_EQ(TRACE_EVENT_PHASE_END, event->phase);
  CHECK_EQ(thread_id, event->thread_id);
  CHECK_EQ(process_id, event->process_id);
  CHECK_GE(event->timestamp, timestamp);
  timestamp = event->timestamp;

  event = harness.get_event(5);
  CHECK_EQ(TRACE_EVENT_PHASE_END, event->phase);
  CHECK_EQ(thread_id, event->thread_id);
  CHECK_EQ(process_id, event->process_id);
  CHECK_GE(event->timestamp, timestamp);
  timestamp = event->timestamp;

  CHECK_EQ(6, harness.events_size());
}

TEST(TracingPerfetto) {
  ::perfetto::TraceConfig perfetto_trace_config;
  perfetto_trace_config.add_buffers()->set_size_kb(4096);
  auto* ds_config = perfetto_trace_config.add_data_sources()->mutable_config();
  ds_config->set_name("v8.trace_events");

  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("v8.trace_events");
  TestDataSource::Register(dsd);

  auto tracing_session_ =
      perfetto::Tracing::NewTrace(perfetto::BackendType::kInProcessBackend);
  tracing_session_->Setup(perfetto_trace_config);
  tracing_session_->Start();
  TestDataSource::started_.Wait();

  for (int i = 0; i < 15; i++) {
    TestDataSource::Trace([&](TestDataSource::TraceContext ctx) {
      auto packet = ctx.NewTracePacket();
      auto* trace_event_bundle = packet->set_chrome_events();
      auto* trace_event = trace_event_bundle->add_trace_events();

      trace_event->set_phase('c');
      trace_event->set_thread_id(v8::base::OS::GetCurrentThreadId());
      trace_event->set_timestamp(123);
      trace_event->set_process_id(v8::base::OS::GetCurrentProcessId());
      trace_event->set_thread_timestamp(123);
    });
  }
  v8::base::Semaphore stopped_{0};
  tracing_session_->SetOnStopCallback([&stopped_]() { stopped_.Signal(); });
  tracing_session_->Stop();
  stopped_.Wait();

  std::ostringstream perfetto_json_stream_;

  {
    v8::platform::tracing::JSONTraceEventListener json_listener_(
        &perfetto_json_stream_);

    std::vector<char> trace = tracing_session_->ReadTraceBlocking();
    json_listener_.ParseFromArray(trace);
  }

  printf("%s\n", perfetto_json_stream_.str().c_str());
  CHECK_GT(perfetto_json_stream_.str().length(), 0);
}

#endif  // V8_USE_PERFETTO

}  // namespace tracing
}  // namespace platform
}  // namespace v8
