// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;

// A bounded readable prefix followed by an inaccessible page. The default is
// exactly eight bytes; extension tests also expose real 176/184-byte tables.
// Reading beyond a rejected prefix terminates the test.
internal sealed unsafe class GuardedHeader : IDisposable
{
    readonly nint allocation;readonly nuint length;public uint* Header {get;}
    public GuardedHeader(int readableBytes=8)
    {
        var page=Environment.SystemPageSize;length=(nuint)(page*2);
        if(readableBytes<8 || readableBytes>page)throw new ArgumentOutOfRangeException(nameof(readableBytes));
        if(OperatingSystem.IsWindows()) {
            allocation=VirtualAlloc(0,length,0x3000,4);
            if(allocation==0 || !VirtualProtect(allocation+page,(nuint)page,1,out _))throw new InvalidOperationException("Guard allocation failed.");
        } else {
            allocation=mmap(0,length,3,0x22,-1,0);
            if(allocation==-1 || mprotect(allocation+page,(nuint)page,0)!=0)throw new InvalidOperationException("Guard allocation failed.");
        }
        Header=(uint*)(allocation+page-readableBytes);
    }
    public void Dispose() {if(OperatingSystem.IsWindows())VirtualFree(allocation,0,0x8000);else munmap(allocation,length);}
    [DllImport("kernel32",SetLastError=true)]static extern nint VirtualAlloc(nint address,nuint bytes,uint allocation,uint protection);
    [DllImport("kernel32",SetLastError=true)][return:MarshalAs(UnmanagedType.Bool)]static extern bool VirtualProtect(nint address,nuint bytes,uint protection,out uint old);
    [DllImport("kernel32",SetLastError=true)][return:MarshalAs(UnmanagedType.Bool)]static extern bool VirtualFree(nint address,nuint bytes,uint kind);
    [DllImport("libc",SetLastError=true)]static extern nint mmap(nint address,nuint bytes,int protection,int flags,int fd,nint offset);
    [DllImport("libc",SetLastError=true)]static extern int mprotect(nint address,nuint bytes,int protection);
    [DllImport("libc",SetLastError=true)]static extern int munmap(nint address,nuint bytes);
}
