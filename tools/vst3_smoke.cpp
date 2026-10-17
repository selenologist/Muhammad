// SPDX-License-Identifier: GPL-3.0-only
// VST3 wrapper smoke test: dlopens a clap-wrapper-built muhammad.vst3,
// runs the factory protocol and initializes the controller - which loads
// muhammad.clap from ~/.clap for real. Build (needs vst3sdk + wrapper built):
//   g++ -std=c++17 -DRELEASE -o vst3_smoke tools/vst3_smoke.cpp \
//     -I <vst3sdk> -I <vst3sdk>/public.sdk -I <vst3sdk>/pluginterfaces -I <vst3sdk>/base \
//     <wrapper-build>/libbase-sdk-vst3.a -ldl -lpthread
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "base/source/fobject.h"

// minimal host context: answers every queryInterface with no-support
struct ProbeHost : public Steinberg::FObject
{
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID, void **) override
    {   return Steinberg::kNoInterface; }
};

int main(int argc, char **argv)
{
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { std::printf("dlopen: %s\n", dlerror()); return 1; }
    auto getF = (Steinberg::IPluginFactory *(*)())dlsym(h, "GetPluginFactory");
    if (!getF) { std::printf("no GetPluginFactory\n"); return 1; }
    auto *f = getF();
    if (!f) { std::printf("null factory\n"); return 1; }
    {
        Steinberg::PFactoryInfo fi{};
        if (f->getFactoryInfo(&fi) != Steinberg::kResultTrue) { std::printf("no factory info\n"); return 1; }
        std::printf("factory vendor: %s\n", fi.vendor);
        const int nc = f->countClasses();
        for (int j = 0; j < nc; j++)
        {
            Steinberg::PClassInfo ci{};
            if (f->getClassInfo(j, &ci) != Steinberg::kResultTrue) continue;
            std::printf("  class: %s  cat=%s\n", (const char *)ci.name, (const char *)ci.category);
            void *obj = nullptr;
            if (f->createInstance(ci.cid, Steinberg::Vst::IEditController::iid, &obj) == Steinberg::kResultTrue && obj)
            {
                std::printf("  createInstance(IEditController) OK\n");
                auto *ctrl = static_cast<Steinberg::Vst::IEditController *>(obj);
                Steinberg::IPluginBase *pb = nullptr;
                if (ctrl->queryInterface(Steinberg::IPluginBase::iid, (void **)&pb) == Steinberg::kResultTrue && pb)
                {
                    auto *ctx = new ProbeHost();
                    const auto rc = pb->initialize(ctx);
                    ctx->release();
                    std::printf("  IPluginBase::initialize -> %d  (clap load happened if 0)\n", (int)rc);
                    pb->terminate();
                    pb->release();
                }
                else std::printf("  queryInterface(IPluginBase) failed\n");
                ctrl->release();
            }
            else std::printf("  createInstance(IEditController) failed\n");
        }
    }
    return 0;
}
