/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jni.h"

#include <android-base/logging.h>
#include <android-base/macros.h>

#include "art_field.h"
#include "art_method-inl.h"
#include "base/enums.h"
#include "common_throws.h"
#include "dex/dex_file-inl.h"
#include "instrumentation.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/profiling_info.h"
#include "jni/jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/class.h"
#include "nativehelper/ScopedUtfChars.h"
#include "oat.h"
#include "oat_file.h"
#include "oat_quick_method_header.h"
#include "profile/profile_compilation_info.h"
#include "runtime.h"
#include "scoped_thread_state_change-inl.h"
#include "scoped_thread_state_change.h"
#include "thread-current-inl.h"

namespace art {

// public static native boolean hasJit();

static jit::Jit* GetJitIfEnabled() {
  Runtime* runtime = Runtime::Current();
  bool can_jit =
      runtime != nullptr
      && runtime->GetJit() != nullptr
      && runtime->GetInstrumentation()->GetCurrentInstrumentationLevel() !=
            instrumentation::Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter;
  return can_jit ? runtime->GetJit() : nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasJit(JNIEnv*, jclass) {
  return GetJitIfEnabled() != nullptr;
}

// public static native boolean hasOatFile();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasOatFile(JNIEnv* env, jclass cls) {
  ScopedObjectAccess soa(env);

  ObjPtr<mirror::Class> klass = soa.Decode<mirror::Class>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  return (oat_dex_file != nullptr) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jobject JNICALL Java_Main_getCompilerFilter(JNIEnv* env,
                                                                 jclass caller ATTRIBUTE_UNUSED,
                                                                 jclass cls) {
  ScopedObjectAccess soa(env);

  ObjPtr<mirror::Class> klass = soa.Decode<mirror::Class>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    return nullptr;
  }

  std::string filter =
      CompilerFilter::NameOfFilter(oat_dex_file->GetOatFile()->GetCompilerFilter());
  return soa.AddLocalReference<jobject>(
      mirror::String::AllocFromModifiedUtf8(soa.Self(), filter.c_str()));
}

// public static native boolean runtimeIsSoftFail();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_runtimeIsSoftFail(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                  jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->IsVerificationSoftFail() ? JNI_TRUE : JNI_FALSE;
}

// public static native boolean hasImage();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasImage(JNIEnv* env ATTRIBUTE_UNUSED,
                                                         jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->GetHeap()->HasBootImageSpace();
}

// public static native boolean isImageDex2OatEnabled();

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isImageDex2OatEnabled(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                      jclass cls ATTRIBUTE_UNUSED) {
  return Runtime::Current()->IsImageDex2OatEnabled();
}

// public static native boolean compiledWithOptimizing();
// Did we use the optimizing compiler to compile this?

extern "C" JNIEXPORT jboolean JNICALL Java_Main_compiledWithOptimizing(JNIEnv* env, jclass cls) {
  ScopedObjectAccess soa(env);

  ObjPtr<mirror::Class> klass = soa.Decode<mirror::Class>(cls);
  const DexFile& dex_file = klass->GetDexFile();
  const OatDexFile* oat_dex_file = dex_file.GetOatDexFile();
  if (oat_dex_file == nullptr) {
    // Could be JIT, which also uses optimizing, but conservatively say no.
    return JNI_FALSE;
  }
  const OatFile* oat_file = oat_dex_file->GetOatFile();
  CHECK(oat_file != nullptr);

  const char* cmd_line = oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kDex2OatCmdLineKey);
  if (cmd_line == nullptr) {
    // Vdex-only execution, conservatively say no.
    return JNI_FALSE;
  }

  // Check the backend.
  constexpr const char* kCompilerBackend = "--compiler-backend=";
  const char* backend = strstr(cmd_line, kCompilerBackend);
  if (backend != nullptr) {
    // If it's set, make sure it's optimizing.
    backend += strlen(kCompilerBackend);
    if (strncmp(backend, "Optimizing", strlen("Optimizing")) != 0) {
      return JNI_FALSE;
    }
  }

  // Check the filter.
  constexpr const char* kCompilerFilter = "--compiler-filter=";
  const char* filter = strstr(cmd_line, kCompilerFilter);
  if (filter != nullptr) {
    filter += strlen(kCompilerFilter);
    const char* end = strchr(filter, ' ');
    std::string string_filter(filter, (end == nullptr) ? strlen(filter) : end - filter);
    CompilerFilter::Filter compiler_filter;
    bool success = CompilerFilter::ParseCompilerFilter(string_filter.c_str(), &compiler_filter);
    CHECK(success);
    return CompilerFilter::IsAotCompilationEnabled(compiler_filter) ? JNI_TRUE : JNI_FALSE;
  }

  // No filter passed, assume default has AOT.
  return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isAotCompiled(JNIEnv* env,
                                                              jclass,
                                                              jclass cls,
                                                              jstring method_name) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  ScopedUtfChars chars(env, method_name);
  CHECK(chars.c_str() != nullptr);
  ArtMethod* method = soa.Decode<mirror::Class>(cls)->FindDeclaredDirectMethodByName(
        chars.c_str(), kRuntimePointerSize);
  const void* oat_code = method->GetOatMethodQuickCode(kRuntimePointerSize);
  if (oat_code == nullptr) {
    return false;
  }
  const void* actual_code = Runtime::Current()->GetInstrumentation()->GetCodeForInvoke(method);
  return actual_code == oat_code;
}

static ArtMethod* GetMethod(ScopedObjectAccess& soa, jclass cls, const ScopedUtfChars& chars)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  CHECK(chars.c_str() != nullptr);
  ArtMethod* method = soa.Decode<mirror::Class>(cls)->FindDeclaredDirectMethodByName(
        chars.c_str(), kRuntimePointerSize);
  if (method == nullptr) {
    method = soa.Decode<mirror::Class>(cls)->FindDeclaredVirtualMethodByName(
        chars.c_str(), kRuntimePointerSize);
  }
  DCHECK(method != nullptr) << "Unable to find method called " << chars.c_str();
  return method;
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasJitCompiledEntrypoint(JNIEnv* env,
                                                                         jclass,
                                                                         jclass cls,
                                                                         jstring method_name) {
  jit::Jit* jit = GetJitIfEnabled();
  if (jit == nullptr) {
    return false;
  }
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  ScopedUtfChars chars(env, method_name);
  ArtMethod* method = GetMethod(soa, cls, chars);
  ScopedAssertNoThreadSuspension sants(__FUNCTION__);
  return jit->GetCodeCache()->ContainsPc(
      Runtime::Current()->GetInstrumentation()->GetCodeForInvoke(method));
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasJitCompiledCode(JNIEnv* env,
                                                                   jclass,
                                                                   jclass cls,
                                                                   jstring method_name) {
  jit::Jit* jit = GetJitIfEnabled();
  if (jit == nullptr) {
    return false;
  }
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  ScopedUtfChars chars(env, method_name);
  ArtMethod* method = GetMethod(soa, cls, chars);
  return jit->GetCodeCache()->ContainsMethod(method);
}

static void ForceJitCompiled(Thread* self,
                             ArtMethod* method,
                             CompilationKind kind) REQUIRES(!Locks::mutator_lock_) {
  // TODO(mythria): Update this check once we support method entry / exit hooks directly from
  // JIT code instead of installing EntryExit stubs.
  if (Runtime::Current()->GetInstrumentation()->EntryExitStubsInstalled() &&
      (method->IsNative() || !Runtime::Current()->IsJavaDebuggable())) {
    return;
  }

  {
    ScopedObjectAccess soa(self);
    if (Runtime::Current()->GetInstrumentation()->IsDeoptimized(method)) {
      std::string msg(method->PrettyMethod());
      msg += ": is not safe to jit!";
      ThrowIllegalStateException(msg.c_str());
      return;
    }
    // We force visible initialization of the declaring class to make sure the method
    // doesn't keep the resolution stub as entrypoint.
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_klass(hs.NewHandle(method->GetDeclaringClass()));
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    if (!class_linker->EnsureInitialized(self, h_klass, true, true)) {
      self->AssertPendingException();
      return;
    }
    if (UNLIKELY(!h_klass->IsInitialized())) {
      // Must be initializing in this thread.
      CHECK_EQ(h_klass->GetStatus(), ClassStatus::kInitializing);
      CHECK_EQ(h_klass->GetClinitThreadId(), self->GetTid());
      std::string msg(method->PrettyMethod());
      msg += ": is not safe to jit because the class is being initialized in this thread!";
      ThrowIllegalStateException(msg.c_str());
      return;
    }
    if (!h_klass->IsVisiblyInitialized()) {
      ScopedThreadSuspension sts(self, ThreadState::kNative);
      class_linker->MakeInitializedClassesVisiblyInitialized(self, /*wait=*/ true);
    }
  }
  jit::Jit* jit = GetJitIfEnabled();
  jit::JitCodeCache* code_cache = jit->GetCodeCache();
  // Update the code cache to make sure the JIT code does not get deleted.
  // Note: this will apply to all JIT compilations.
  code_cache->SetGarbageCollectCode(false);
  do {
    // Sleep to yield to the compiler thread.
    usleep(1000);
    ScopedObjectAccess soa(self);
    // Will either ensure it's compiled or do the compilation itself. We do
    // this before checking if we will execute JIT code in case the request
    // is for an 'optimized' compilation.
    jit->CompileMethod(method, self, kind, /*prejit=*/ false);
    const void* entry_point = method->GetEntryPointFromQuickCompiledCode();
    if (code_cache->ContainsPc(entry_point)) {
      // If we're running baseline or not requesting optimized, we're good to go.
      if (jit->GetJitCompiler()->IsBaselineCompiler() || kind != CompilationKind::kOptimized) {
        break;
      }
      // If we're requesting optimized, check that we did get the method
      // compiled optimized.
      OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromEntryPoint(entry_point);
      if (!CodeInfo::IsBaseline(method_header->GetOptimizedCodeInfoPtr())) {
        break;
      }
    }
  } while (true);
}

extern "C" JNIEXPORT void JNICALL Java_Main_ensureMethodJitCompiled(JNIEnv*, jclass, jobject meth) {
  jit::Jit* jit = GetJitIfEnabled();
  if (jit == nullptr) {
    return;
  }

  Thread* self = Thread::Current();
  ArtMethod* method;
  {
    ScopedObjectAccess soa(self);
    method = ArtMethod::FromReflectedMethod(soa, meth);
  }
  ForceJitCompiled(self, method, CompilationKind::kOptimized);
}

extern "C" JNIEXPORT void JNICALL Java_Main_ensureJitCompiled(JNIEnv* env,
                                                             jclass,
                                                             jclass cls,
                                                             jstring method_name) {
  jit::Jit* jit = GetJitIfEnabled();
  if (jit == nullptr) {
    return;
  }

  Thread* self = Thread::Current();
  ArtMethod* method = nullptr;
  {
    ScopedObjectAccess soa(self);

    ScopedUtfChars chars(env, method_name);
    method = GetMethod(soa, cls, chars);
  }
  ForceJitCompiled(self, method, CompilationKind::kOptimized);
}

extern "C" JNIEXPORT void JNICALL Java_Main_ensureJitBaselineCompiled(JNIEnv* env,
                                                                      jclass,
                                                                      jclass cls,
                                                                      jstring method_name) {
  jit::Jit* jit = GetJitIfEnabled();
  if (jit == nullptr) {
    return;
  }

  Thread* self = Thread::Current();
  ArtMethod* method = nullptr;
  {
    ScopedObjectAccess soa(self);

    ScopedUtfChars chars(env, method_name);
    method = GetMethod(soa, cls, chars);
  }
  ForceJitCompiled(self, method, CompilationKind::kBaseline);
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_hasSingleImplementation(JNIEnv* env,
                                                                        jclass,
                                                                        jclass cls,
                                                                        jstring method_name) {
  ArtMethod* method = nullptr;
  ScopedObjectAccess soa(Thread::Current());
  ScopedUtfChars chars(env, method_name);
  CHECK(chars.c_str() != nullptr);
  method = soa.Decode<mirror::Class>(cls)->FindDeclaredVirtualMethodByName(
      chars.c_str(), kRuntimePointerSize);
  return method->HasSingleImplementation();
}

extern "C" JNIEXPORT int JNICALL Java_Main_getHotnessCounter(JNIEnv* env,
                                                             jclass,
                                                             jclass cls,
                                                             jstring method_name) {
  ScopedObjectAccess soa(Thread::Current());
  ScopedUtfChars chars(env, method_name);
  CHECK(chars.c_str() != nullptr);
  ArtMethod* method =
      soa.Decode<mirror::Class>(cls)->FindDeclaredDirectMethodByName(chars.c_str(),
                                                                     kRuntimePointerSize);
  if (method != nullptr) {
    return method->GetCounter();
  }

  method = soa.Decode<mirror::Class>(cls)->FindDeclaredVirtualMethodByName(chars.c_str(),
                                                                           kRuntimePointerSize);
  if (method != nullptr) {
    return method->GetCounter();
  }

  return std::numeric_limits<int32_t>::min();
}

extern "C" JNIEXPORT int JNICALL Java_Main_numberOfDeoptimizations(JNIEnv*, jclass) {
  return Runtime::Current()->GetNumberOfDeoptimizations();
}

extern "C" JNIEXPORT void JNICALL Java_Main_fetchProfiles(JNIEnv*, jclass) {
  jit::Jit* jit = GetJitIfEnabled();
  if (jit == nullptr) {
    return;
  }
  jit::JitCodeCache* code_cache = jit->GetCodeCache();
  std::vector<ProfileMethodInfo> unused_vector;
  std::set<std::string> unused_locations;
  unused_locations.insert("fake_location");
  ScopedObjectAccess soa(Thread::Current());
  code_cache->GetProfiledMethods(unused_locations, unused_vector);
}

extern "C" JNIEXPORT void JNICALL Java_Main_waitForCompilation(JNIEnv*, jclass) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    jit->WaitForCompilationToFinish(Thread::Current());
  }
}

extern "C" JNIEXPORT void JNICALL Java_Main_stopJit(JNIEnv*, jclass) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    jit->Stop();
  }
}

extern "C" JNIEXPORT void JNICALL Java_Main_startJit(JNIEnv*, jclass) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    jit->Start();
  }
}

extern "C" JNIEXPORT jint JNICALL Java_Main_getJitThreshold(JNIEnv*, jclass) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  return (jit != nullptr) ? jit->HotMethodThreshold() : 0;
}

extern "C" JNIEXPORT void JNICALL Java_Main_deoptimizeBootImage(JNIEnv*, jclass) {
  ScopedSuspendAll ssa(__FUNCTION__);
  Runtime::Current()->DeoptimizeBootImage();
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isDebuggable(JNIEnv*, jclass) {
  return Runtime::Current()->IsJavaDebuggable() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_Main_setTargetSdkVersion(JNIEnv*, jclass, jint version) {
  Runtime::Current()->SetTargetSdkVersion(static_cast<uint32_t>(version));
}

extern "C" JNIEXPORT jlong JNICALL Java_Main_genericFieldOffset(JNIEnv* env, jclass, jobject fld) {
  jfieldID fid = env->FromReflectedField(fld);
  ScopedObjectAccess soa(env);
  ArtField* af = jni::DecodeArtField(fid);
  return af->GetOffset().Int32Value();
}

extern "C" JNIEXPORT jboolean JNICALL Java_Main_isObsoleteObject(JNIEnv* env, jclass, jclass c) {
  ScopedObjectAccess soa(env);
  return soa.Decode<mirror::Class>(c)->IsObsoleteObject();
}

extern "C" JNIEXPORT void JNICALL Java_Main_forceInterpreterOnThread(JNIEnv* env,
                                                                     jclass cls ATTRIBUTE_UNUSED) {
  ScopedObjectAccess soa(env);
  MutexLock thread_list_mu(soa.Self(), *Locks::thread_list_lock_);
  soa.Self()->IncrementForceInterpreterCount();
}

extern "C" JNIEXPORT void JNICALL Java_Main_setAsyncExceptionsThrown(JNIEnv* env ATTRIBUTE_UNUSED,
                                                                     jclass cls ATTRIBUTE_UNUSED) {
  Runtime::Current()->SetAsyncExceptionsThrown();
}

}  // namespace art
