#include "platform/Thread.h"

#ifdef WII_PLATFORM
#include <ogc/lwp.h>
#elif defined(PS2_PLATFORM)
#include <kernel.h>
#include <malloc.h>
#include <algorithm>
#include <new>
#elif defined(CTR_PLATFORM)
#include <3ds.h>
#include <algorithm>
#include <new>
#else
#include <thread>
#include <functional>
#include <new>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#endif

#ifdef WII_PLATFORM
PlatformThread::PlatformThread() : handle_(static_cast<std::uintptr_t>(LWP_THREAD_NULL)) {}
PlatformThread::~PlatformThread() { if (joinable() && !isCurrent()) join(); }
bool PlatformThread::start(Entry entry, void* argument, std::size_t stackSize, int priority, std::uintptr_t affinityMask)
{
    if (joinable() || entry == nullptr) return false;
    (void)affinityMask;
    lwp_t thread = LWP_THREAD_NULL;
    const s32 result = LWP_CreateThread(&thread, entry, argument, nullptr, static_cast<u32>(stackSize), priority);
    if (result < 0) return false;
    handle_ = static_cast<std::uintptr_t>(thread);
    return true;
}
void PlatformThread::join()
{
    if (!joinable() || isCurrent()) return;
    LWP_JoinThread(static_cast<lwp_t>(handle_), nullptr);
    handle_ = static_cast<std::uintptr_t>(LWP_THREAD_NULL);
}
bool PlatformThread::joinable() const { return static_cast<lwp_t>(handle_) != LWP_THREAD_NULL; }
bool PlatformThread::isCurrent() const { return joinable() && static_cast<lwp_t>(handle_) == LWP_GetSelf(); }
std::uintptr_t PlatformThread::currentId() { return static_cast<std::uintptr_t>(LWP_GetSelf()); }

#elif defined(PS2_PLATFORM)

namespace
{
struct Ps2ThreadStartContext
{
    PlatformThread::Entry entry = nullptr;
    void* argument = nullptr;
    int doneSema = -1;
};

void ps2PlatformThreadEntry(void* raw)
{
    Ps2ThreadStartContext* context = static_cast<Ps2ThreadStartContext*>(raw);
    if (context != nullptr && context->entry != nullptr)
    {
        try
        {
            context->entry(context->argument);
        }
        catch (...)
        {
            // Never unwind a C++ exception through the EE kernel thread entry.
        }
    }

    if (context != nullptr && context->doneSema >= 0)
        SignalSema(context->doneSema);
    ExitThread();
}
}

struct PlatformThread::Impl
{
    int threadId = -1;
    int doneSema = -1;
    void* stack = nullptr;
    Ps2ThreadStartContext* context = nullptr;
};

PlatformThread::PlatformThread() : impl_(new (std::nothrow) Impl()) {}

PlatformThread::~PlatformThread()
{
    if (!impl_)
        return;
    if (joinable() && !isCurrent())
        join();
    delete impl_;
}

bool PlatformThread::start(Entry entry, void* argument, std::size_t stackSize,
                           int priority, std::uintptr_t affinityMask)
{
    if (!impl_ || !entry || joinable())
        return false;
    (void)affinityMask; // EE has one main CPU; no affinity mask is needed.

    const std::size_t actualStackSize = std::max<std::size_t>(stackSize, 4096);
    void* stack = memalign(16, actualStackSize);
    if (!stack)
        return false;

    ee_sema_t semaphore{};
    semaphore.init_count = 0;
    semaphore.max_count = 1;
    semaphore.option = 0;
    const int doneSema = CreateSema(&semaphore);
    if (doneSema < 0)
    {
        free(stack);
        return false;
    }

    Ps2ThreadStartContext* context = new (std::nothrow) Ps2ThreadStartContext();
    if (!context)
    {
        DeleteSema(doneSema);
        free(stack);
        return false;
    }
    context->entry = entry;
    context->argument = argument;
    context->doneSema = doneSema;

    ee_thread_t thread{};
    thread.func = reinterpret_cast<void*>(ps2PlatformThreadEntry);
    thread.stack = stack;
    thread.stack_size = static_cast<int>(actualStackSize);
    thread.gp_reg = &_gp;
    thread.initial_priority = std::clamp(priority, 1, 127);

    const int threadId = CreateThread(&thread);
    if (threadId < 0)
    {
        delete context;
        DeleteSema(doneSema);
        free(stack);
        return false;
    }

    if (StartThread(threadId, context) < 0)
    {
        DeleteThread(threadId);
        delete context;
        DeleteSema(doneSema);
        free(stack);
        return false;
    }

    impl_->threadId = threadId;
    impl_->doneSema = doneSema;
    impl_->stack = stack;
    impl_->context = context;
    return true;
}

void PlatformThread::join()
{
    if (!joinable() || isCurrent())
        return;

    if (impl_->doneSema >= 0)
        WaitSema(impl_->doneSema);

    if (impl_->threadId >= 0)
        DeleteThread(impl_->threadId);
    if (impl_->doneSema >= 0)
        DeleteSema(impl_->doneSema);
    free(impl_->stack);
    delete impl_->context;

    impl_->threadId = -1;
    impl_->doneSema = -1;
    impl_->stack = nullptr;
    impl_->context = nullptr;
}

bool PlatformThread::joinable() const
{
    return impl_ && impl_->threadId >= 0;
}

bool PlatformThread::isCurrent() const
{
    return joinable() && impl_->threadId == GetThreadId();
}

std::uintptr_t PlatformThread::currentId()
{
    return static_cast<std::uintptr_t>(GetThreadId());
}

#elif defined(CTR_PLATFORM)

namespace
{
// threadCreate() wants a void(void*) entry, but PlatformThread::Entry returns
// void*. Carry the pair through a heap context instead of adding a static
// member to Thread.h (the header would then need CTR_PLATFORM too), which is
// the same shape the PS2 branch uses.
struct CtrThreadStartContext
{
    PlatformThread::Entry entry = nullptr;
    void* argument = nullptr;
};

void ctrThreadEntry(void* raw)
{
    CtrThreadStartContext* context = static_cast<CtrThreadStartContext*>(raw);
    if (context != nullptr && context->entry != nullptr)
    {
        try
        {
            context->entry(context->argument);
        }
        catch (...)
        {
            // Never unwind a C++ exception through a libctru thread entry.
        }
    }
}
} // namespace

struct PlatformThread::Impl
{
    Thread thread = nullptr;
    CtrThreadStartContext* context = nullptr;
};

PlatformThread::PlatformThread() : impl_(new (std::nothrow) Impl()) {}

PlatformThread::~PlatformThread()
{
    if (!impl_)
        return;
    if (joinable() && !isCurrent())
        join();
    delete impl_;
}

bool PlatformThread::start(Entry entry, void* argument, std::size_t stackSize,
                           int priority, std::uintptr_t affinityMask)
{
    if (!impl_ || !entry || joinable())
        return false;
    (void)affinityMask; // Phase 1 pins to the default core; see core_id below.

    CtrThreadStartContext* context = new (std::nothrow) CtrThreadStartContext();
    if (!context)
        return false;
    context->entry = entry;
    context->argument = argument;

    // libctru counts the other way round -- low value is high priority -- and
    // only accepts [0x18;0x3F], with the main thread at 0x30. The shared
    // signature's 64 means "normal" (the std::thread branch maps below/above
    // 64 onto BELOW/ABOVE_NORMAL), so anchor it on the main thread's own
    // priority and let larger shared values become smaller 3DS ones, clamped
    // into the userland range. A raw 64 would land at 0x40, one past the top.
    int prio = 0x30 + (64 - priority);
    if (prio < 0x18)
        prio = 0x18;
    else if (prio > 0x3F)
        prio = 0x3F;

    // -2 = the CPU the Exheader selects (core 0 on Old 3DS, and legal there;
    // running on core 1 would need APT_SetAppCpuTimeLimit first, and on New 3DS
    // cores 2/3 need kernel flags -- deferred to a later phase).
    Thread thread = threadCreate(ctrThreadEntry, context, stackSize, prio, -2, false);
    if (thread == nullptr)
    {
        delete context;
        return false;
    }

    impl_->thread = thread;
    impl_->context = context;
    return true;
}

void PlatformThread::join()
{
    if (!joinable() || isCurrent())
        return;

    threadJoin(impl_->thread, U64_MAX);
    // Not detached, so the handle has to be released explicitly; threadFree on
    // a detached thread would double-free it.
    threadFree(impl_->thread);
    delete impl_->context;

    impl_->thread = nullptr;
    impl_->context = nullptr;
}

bool PlatformThread::joinable() const { return impl_ && impl_->thread != nullptr; }

bool PlatformThread::isCurrent() const { return joinable() && impl_->thread == threadGetCurrent(); }

std::uintptr_t PlatformThread::currentId()
{
    // NULL for the main thread, which therefore reads as 0. That is still a
    // usable identity: every live PlatformThread has a non-null handle, so the
    // main thread is distinguishable from all of them, and each worker from
    // every other. WorldLoadTrace only ever captures the game thread here.
    return reinterpret_cast<std::uintptr_t>(threadGetCurrent());
}

#else
struct PlatformThread::Impl { std::thread thread; };
PlatformThread::PlatformThread() : impl_(new (std::nothrow) Impl()) {}
PlatformThread::~PlatformThread() { if (impl_) { if (joinable() && !isCurrent()) join(); delete impl_; } }
bool PlatformThread::start(Entry entry, void* argument, std::size_t, int priority, std::uintptr_t affinityMask)
{
    if (!impl_ || !entry || joinable()) return false;
    try
    {
        impl_->thread = std::thread([entry, argument, priority, affinityMask]()
        {
#if defined(_WIN32)
            if (priority < 64)
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            else if (priority > 64)
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            if (affinityMask != 0)
                SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(affinityMask));
#else
            (void)priority;
            (void)affinityMask;
#endif
            entry(argument);
        });
        return true;
    }
    catch (...)
    {
        return false;
    }
}
void PlatformThread::join() { if (joinable() && !isCurrent()) impl_->thread.join(); }
bool PlatformThread::joinable() const { return impl_ && impl_->thread.joinable(); }
bool PlatformThread::isCurrent() const { return joinable() && impl_->thread.get_id() == std::this_thread::get_id(); }
std::uintptr_t PlatformThread::currentId() { return std::hash<std::thread::id>()(std::this_thread::get_id()); }

#endif
