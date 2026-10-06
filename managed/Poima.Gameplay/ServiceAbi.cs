// SPDX-License-Identifier: Apache-2.0
namespace Poima;

// The compiled struct is the epoch-7 baseline prefix. A host may append fields;
// this SDK neither reads nor validates those unknown extension bytes.
internal static unsafe class ServiceAbi
{
    internal const uint Epoch=7, RequiredBytes=176;
    internal static void Validate(NativeServices* services,GameInput* inputs,uint inputCount)
    {
        // Read only the stable eight-byte header until its advertised length
        // establishes that every baseline callback slot is available.
        if(services==null || services->Version!=Epoch || services->Bytes<RequiredBytes ||
            inputCount>32 || (inputCount>0 && inputs==null))
            throw new ArgumentException("Gameplay service ABI mismatch: services epoch 7 with at least 176 bytes required.");
        if(services->Entity==null || services->Raycast==null || services->Move==null || services->Sound==null ||
            services->AnimationGet==null || services->AnimationSet==null || services->SaveInfo==null ||
            services->SaveRequest==null || services->SaveResult==null || services->ComponentQuery==null ||
            services->ComponentGet==null || services->ComponentSet==null || services->EntityAlive==null ||
            services->Spawn==null || services->Despawn==null || services->TemplateComponentGet==null ||
            services->UiGet==null || services->UiEdit==null || services->ControlInfo==null || services->ControlRequest==null)
            throw new ArgumentException("Gameplay service callback is absent.");
    }
}
