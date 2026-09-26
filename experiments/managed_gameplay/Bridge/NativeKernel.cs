// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using Poima.Lab;

namespace Poima.ManagedLab;

public sealed unsafe class NativeKernel
{
    private readonly nint library;
    private readonly delegate* unmanaged[Cdecl]<ulong, void*> allocate;
    internal readonly delegate* unmanaged[Cdecl]<void*, void> Free;
    private readonly delegate* unmanaged[Cdecl]<ulong> liveBytes;
    private readonly delegate* unmanaged[Cdecl]<ulong> nativeCalls;
    public NativeServices Services { get; }

    public NativeKernel(string path)
    {
        // Process-lifetime native service library. State buffers have independent
        // deterministic disposal; no gameplay assembly owns this library.
        library = NativeLibrary.Load(Path.GetFullPath(path));
        var abi = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(library, "poima_lab_abi");
        if (abi() != 1) throw new InvalidOperationException("Native kernel ABI mismatch.");
        allocate = (delegate* unmanaged[Cdecl]<ulong, void*>)NativeLibrary.GetExport(library, "poima_lab_allocate");
        Free = (delegate* unmanaged[Cdecl]<void*, void>)NativeLibrary.GetExport(library, "poima_lab_free");
        liveBytes = (delegate* unmanaged[Cdecl]<ulong>)NativeLibrary.GetExport(library, "poima_lab_live_bytes");
        nativeCalls = (delegate* unmanaged[Cdecl]<ulong>)NativeLibrary.GetExport(library, "poima_lab_native_calls");
        Services = new NativeServices((delegate* unmanaged[Cdecl]<ulong, ulong, ulong>)NativeLibrary.GetExport(library, "poima_lab_add"));
    }
    public ulong LiveBytes => liveBytes();
    public ulong NativeCalls => nativeCalls();
    public NativeBuffer Allocate(int bytes)
    {
        if (bytes <= 0) throw new ArgumentOutOfRangeException(nameof(bytes));
        var pointer = allocate((ulong)bytes);
        if (pointer is null) throw new OutOfMemoryException("Native fixture allocation failed.");
        return new NativeBuffer(this, (nint)pointer, bytes);
    }
}

public sealed unsafe class NativeBuffer : SafeHandleZeroOrMinusOneIsInvalid
{
    private readonly NativeKernel kernel;
    public int Bytes { get; }
    internal NativeBuffer(NativeKernel kernel, nint pointer, int bytes) : base(true)
    {
        this.kernel = kernel;
        Bytes = bytes;
        SetHandle(pointer);
    }
    public Span<byte> Span
    {
        get
        {
            ObjectDisposedException.ThrowIf(IsClosed, this);
            return new Span<byte>((void*)handle, Bytes);
        }
    }
    public NativeBuffer Copy()
    {
        var copy = kernel.Allocate(Bytes);
        Span.CopyTo(copy.Span);
        GC.KeepAlive(this);
        return copy;
    }
    protected override bool ReleaseHandle() { kernel.Free((void*)handle); return true; }
}
