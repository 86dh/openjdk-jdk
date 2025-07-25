/*
 * Copyright (c) 1999, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_COMPILER_COMPILEBROKER_HPP
#define SHARE_COMPILER_COMPILEBROKER_HPP

#include "ci/compilerInterface.hpp"
#include "compiler/abstractCompiler.hpp"
#include "compiler/compilerDirectives.hpp"
#include "compiler/compilerThread.hpp"
#include "compiler/compileTask.hpp"
#include "runtime/atomic.hpp"
#include "runtime/perfDataTypes.hpp"
#include "utilities/stack.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmciCompiler.hpp"
#endif

class nmethod;

#if defined(ASSERT) && COMPILER2_OR_JVMCI
// Stress testing. Dedicated threads revert optimizations based on escape analysis concurrently to
// the running java application.  Configured with vm options DeoptimizeObjectsALot*.
class DeoptimizeObjectsALotThread : public JavaThread {

  static void deopt_objs_alot_thread_entry(JavaThread* thread, TRAPS);
  void deoptimize_objects_alot_loop_single();
  void deoptimize_objects_alot_loop_all();

public:
  DeoptimizeObjectsALotThread() : JavaThread(&deopt_objs_alot_thread_entry) { }

  bool is_hidden_from_external_view() const      { return true; }
};
#endif

// CompilerCounters
//
// Per Compiler Performance Counters.
//
class CompilerCounters : public CHeapObj<mtCompiler> {

  public:
    enum {
      cmname_buffer_length = 160
    };

  private:

    char _current_method[cmname_buffer_length];
    int  _compile_type;

  public:
    CompilerCounters();

    // these methods should be called in a thread safe context

    void set_current_method(const char* method) {
      strncpy(_current_method, method, (size_t)cmname_buffer_length-1);
      _current_method[cmname_buffer_length-1] = '\0';
    }

    char* current_method()                  { return _current_method; }

    void set_compile_type(int compile_type) {
      _compile_type = compile_type;
    }

    int compile_type()                       { return _compile_type; }

};

// CompileQueue
//
// A list of CompileTasks.
class CompileQueue : public CHeapObj<mtCompiler> {
 private:
  const char* _name;

  CompileTask* _first;
  CompileTask* _last;

  CompileTask* _first_stale;

  volatile int _size;
  int _peak_size;
  uint _total_added;
  uint _total_removed;

  void purge_stale_tasks();
 public:
  CompileQueue(const char* name) {
    _name = name;
    _first = nullptr;
    _last = nullptr;
    _size = 0;
    _total_added = 0;
    _total_removed = 0;
    _peak_size = 0;
    _first_stale = nullptr;
  }

  const char*  name() const                      { return _name; }

  void         add(CompileTask* task);
  void         remove(CompileTask* task);
  void         remove_and_mark_stale(CompileTask* task);
  CompileTask* first()                           { return _first; }
  CompileTask* last()                            { return _last;  }

  CompileTask* get(CompilerThread* thread);

  bool         is_empty() const                  { return _first == nullptr; }
  int          size()     const                  { return _size;          }

  int         get_peak_size()     const          { return _peak_size; }
  uint        get_total_added()   const          { return _total_added; }
  uint        get_total_removed() const          { return _total_removed; }

  // Redefine Classes support
  void mark_on_stack();
  void delete_all();
  void print_tty();
  void print(outputStream* st = tty);

  ~CompileQueue() {
    assert (is_empty(), " Compile Queue must be empty");
  }
};

// CompileTaskWrapper
//
// Assign this task to the current thread.  Deallocate the task
// when the compilation is complete.
class CompileTaskWrapper : StackObj {
public:
  CompileTaskWrapper(CompileTask* task);
  ~CompileTaskWrapper();
};

// Compilation
//
// The broker for all compilation requests.
class CompileBroker: AllStatic {
 friend class Threads;
 friend class CompileTaskWrapper;

 public:
  enum {
    name_buffer_length = 100
  };

  // Compile type Information for print_last_compile() and CompilerCounters
  enum { no_compile, normal_compile, osr_compile, native_compile };
  static int assign_compile_id (const methodHandle& method, int osr_bci);


 private:
  static bool _initialized;
  static volatile bool _should_block;

  // This flag can be used to stop compilation or turn it back on
  static volatile jint _should_compile_new_jobs;

  // The installed compiler(s)
  static AbstractCompiler* _compilers[2];

  // The maximum numbers of compiler threads to be determined during startup.
  static int _c1_count, _c2_count;

  // An array of compiler thread Java objects
  static jobject *_compiler1_objects, *_compiler2_objects;

  // An array of compiler logs
  static CompileLog **_compiler1_logs, **_compiler2_logs;

  // These counters are used for assigning id's to each compilation
  static volatile jint _compilation_id;
  static volatile jint _osr_compilation_id;
  static volatile jint _native_compilation_id;

  static CompileQueue* _c2_compile_queue;
  static CompileQueue* _c1_compile_queue;

  // performance counters
  static PerfCounter* _perf_total_compilation;
  static PerfCounter* _perf_osr_compilation;
  static PerfCounter* _perf_standard_compilation;

  static PerfCounter* _perf_total_bailout_count;
  static PerfCounter* _perf_total_invalidated_count;
  static PerfCounter* _perf_total_compile_count;
  static PerfCounter* _perf_total_osr_compile_count;
  static PerfCounter* _perf_total_standard_compile_count;

  static PerfCounter* _perf_sum_osr_bytes_compiled;
  static PerfCounter* _perf_sum_standard_bytes_compiled;
  static PerfCounter* _perf_sum_nmethod_size;
  static PerfCounter* _perf_sum_nmethod_code_size;

  static PerfStringVariable* _perf_last_method;
  static PerfStringVariable* _perf_last_failed_method;
  static PerfStringVariable* _perf_last_invalidated_method;
  static PerfVariable*       _perf_last_compile_type;
  static PerfVariable*       _perf_last_compile_size;
  static PerfVariable*       _perf_last_failed_type;
  static PerfVariable*       _perf_last_invalidated_type;

  // Timers and counters for generating statistics
  static elapsedTimer _t_total_compilation;
  static elapsedTimer _t_osr_compilation;
  static elapsedTimer _t_standard_compilation;
  static elapsedTimer _t_invalidated_compilation;
  static elapsedTimer _t_bailedout_compilation;

  static uint _total_compile_count;
  static uint _total_bailout_count;
  static uint _total_invalidated_count;
  static uint _total_native_compile_count;
  static uint _total_osr_compile_count;
  static uint _total_standard_compile_count;
  static uint _total_compiler_stopped_count;
  static uint _total_compiler_restarted_count;
  static uint _sum_osr_bytes_compiled;
  static uint _sum_standard_bytes_compiled;
  static uint _sum_nmethod_size;
  static uint _sum_nmethod_code_size;
  static jlong _peak_compilation_time;

  static CompilerStatistics _stats_per_level[];

  static volatile int _print_compilation_warning;

  enum ThreadType {
    compiler_t,
    deoptimizer_t,
    training_replay_t
  };

  static JavaThread* make_thread(ThreadType type, jobject thread_oop, CompileQueue* queue, AbstractCompiler* comp, JavaThread* THREAD);
  static void init_compiler_threads();
  static void init_training_replay();
  static void possibly_add_compiler_threads(JavaThread* THREAD);
  static bool compilation_is_prohibited(const methodHandle& method, int osr_bci, int comp_level, bool excluded);

  static CompileTask* create_compile_task(CompileQueue*       queue,
                                          int                 compile_id,
                                          const methodHandle& method,
                                          int                 osr_bci,
                                          int                 comp_level,
                                          int                 hot_count,
                                          CompileTask::CompileReason compile_reason,
                                          bool                blocking);
  static void wait_for_completion(CompileTask* task);
#if INCLUDE_JVMCI
  static bool wait_for_jvmci_completion(JVMCICompiler* comp, CompileTask* task, JavaThread* thread);
#endif

  static void free_buffer_blob_if_allocated(CompilerThread* thread);

  static void invoke_compiler_on_method(CompileTask* task);
  static void handle_compile_error(CompilerThread* thread, CompileTask* task, ciEnv* ci_env,
                                   int compilable, const char* failure_reason);
  static void update_compile_perf_data(CompilerThread *thread, const methodHandle& method, bool is_osr);

  static void collect_statistics(CompilerThread* thread, elapsedTimer time, CompileTask* task);

  static void compile_method_base(const methodHandle& method,
                                  int osr_bci,
                                  int comp_level,
                                  int hot_count,
                                  CompileTask::CompileReason compile_reason,
                                  bool blocking,
                                  Thread* thread);

  static CompileQueue* compile_queue(int comp_level);
  static bool init_compiler_runtime();
  static void shutdown_compiler_runtime(AbstractCompiler* comp, CompilerThread* thread);

public:
  enum {
    // The entry bci used for non-OSR compilations.
    standard_entry_bci = InvocationEntryBci
  };

  static AbstractCompiler* compiler(int comp_level) {
    if (is_c2_compile(comp_level)) return _compilers[1]; // C2
    if (is_c1_compile(comp_level)) return _compilers[0]; // C1
    return nullptr;
  }

  static bool compilation_is_complete(const methodHandle& method, int osr_bci, int comp_level);
  static bool compilation_is_in_queue(const methodHandle& method);
  static void print_compile_queues(outputStream* st);
  static int queue_size(int comp_level) {
    CompileQueue *q = compile_queue(comp_level);
    return q != nullptr ? q->size() : 0;
  }
  static void compilation_init(JavaThread* THREAD);
  static void init_compiler_thread_log();
  static nmethod* compile_method(const methodHandle& method,
                                 int osr_bci,
                                 int comp_level,
                                 int hot_count,
                                 CompileTask::CompileReason compile_reason,
                                 TRAPS);
  static CompileQueue* c1_compile_queue();
  static CompileQueue* c2_compile_queue();

private:
  static nmethod* compile_method(const methodHandle& method,
                                   int osr_bci,
                                   int comp_level,
                                   int hot_count,
                                   CompileTask::CompileReason compile_reason,
                                   DirectiveSet* directive,
                                   TRAPS);

public:
  // Acquire any needed locks and assign a compile id
  static int assign_compile_id_unlocked(Thread* thread, const methodHandle& method, int osr_bci);

  static void compiler_thread_loop();
  static int get_compilation_id() { return _compilation_id; }

  // Set _should_block.
  // Call this from the VM, with Threads_lock held and a safepoint requested.
  static void set_should_block();

  // Call this from the compiler at convenient points, to poll for _should_block.
  static void maybe_block();

  enum CompilerActivity {
    // Flags for toggling compiler activity
    stop_compilation     = 0,
    run_compilation      = 1,
    shutdown_compilation = 2
  };

  static inline jint get_compilation_activity_mode() { return _should_compile_new_jobs; }
  static inline bool should_compile_new_jobs() { return UseCompiler && (_should_compile_new_jobs == run_compilation); }
  static bool set_should_compile_new_jobs(jint new_state) {
    // Return success if the current caller set it
    jint old = Atomic::cmpxchg(&_should_compile_new_jobs, 1-new_state, new_state);
    bool success = (old == (1-new_state));
    if (success) {
      if (new_state == run_compilation) {
        _total_compiler_restarted_count++;
      } else {
        _total_compiler_stopped_count++;
      }
    }
    return success;
  }

  static void disable_compilation_forever() {
    UseCompiler               = false;
    AlwaysCompileLoopMethods  = false;
    Atomic::xchg(&_should_compile_new_jobs, jint(shutdown_compilation));
  }

  static bool is_compilation_disabled_forever() {
    return Atomic::load(&_should_compile_new_jobs) == shutdown_compilation;
  }

  static void wait_for_no_active_tasks();

  static void handle_full_code_cache(CodeBlobType code_blob_type);
  // Ensures that warning is only printed once.
  static bool should_print_compiler_warning() {
    jint old = Atomic::cmpxchg(&_print_compilation_warning, 0, 1);
    return old == 0;
  }
  // Return total compilation ticks
  static jlong total_compilation_ticks();

  // Redefine Classes support
  static void mark_on_stack();

  // Print current compilation time stats for a given compiler
  static void print_times(const char* name, CompilerStatistics* stats);

  // Print a detailed accounting of compilation time
  static void print_times(bool per_compiler = true, bool aggregate = true);

  // compiler name for debugging
  static const char* compiler_name(int comp_level);

  // Provide access to compiler thread Java objects
  static jobject compiler1_object(int idx) {
    assert(_compiler1_objects != nullptr, "must be initialized");
    assert(idx < _c1_count, "oob");
    return _compiler1_objects[idx];
  }

  static jobject compiler2_object(int idx) {
    assert(_compiler2_objects != nullptr, "must be initialized");
    assert(idx < _c2_count, "oob");
    return _compiler2_objects[idx];
  }

  static AbstractCompiler* compiler1() { return _compilers[0]; }
  static AbstractCompiler* compiler2() { return _compilers[1]; }

  static bool can_remove(CompilerThread *ct, bool do_it);

  static CompileLog* get_log(CompilerThread* ct);

  static int get_c1_thread_count() {                return _compilers[0]->num_compiler_threads(); }
  static int get_c2_thread_count() {                return _compilers[1]->num_compiler_threads(); }
  static int get_total_compile_count() {            return _total_compile_count; }
  static int get_total_bailout_count() {            return _total_bailout_count; }
  static int get_total_invalidated_count() {        return _total_invalidated_count; }
  static int get_total_native_compile_count() {     return _total_native_compile_count; }
  static int get_total_osr_compile_count() {        return _total_osr_compile_count; }
  static int get_total_standard_compile_count() {   return _total_standard_compile_count; }
  static int get_total_compiler_stopped_count() {   return _total_compiler_stopped_count; }
  static int get_total_compiler_restarted_count() { return _total_compiler_restarted_count; }
  static int get_sum_osr_bytes_compiled() {         return _sum_osr_bytes_compiled; }
  static int get_sum_standard_bytes_compiled() {    return _sum_standard_bytes_compiled; }
  static int get_sum_nmethod_size() {               return _sum_nmethod_size;}
  static int get_sum_nmethod_code_size() {          return _sum_nmethod_code_size; }
  static jlong get_peak_compilation_time() {        return _peak_compilation_time; }
  static jlong get_total_compilation_time() {       return _t_total_compilation.milliseconds(); }

  // Log that compilation profiling is skipped because metaspace is full.
  static void log_metaspace_failure();

  // CodeHeap State Analytics.
  static void print_info(outputStream *out);
  static void print_heapinfo(outputStream *out, const char* function, size_t granularity);
};

// In order to achiveve a maximally fast warmup we attempt to compile important methods as soon as all
// the classes that they depend on are initialized. TrainingReplayThread processes a queue of InstanceKlass*
// that have just finished running their static initializers. We find all the methods that depend on the given class
// and for which the number of remaining dependencies is now zero, and eagerly compile them.
class TrainingReplayThread : public JavaThread {
  static void training_replay_thread_entry(JavaThread* thread, TRAPS);
public:
  TrainingReplayThread() : JavaThread(&training_replay_thread_entry) { }

  bool is_hidden_from_external_view() const      { return true; }
};

#endif // SHARE_COMPILER_COMPILEBROKER_HPP
