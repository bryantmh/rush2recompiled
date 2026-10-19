// Times RmlUi's layout of an RML document offline, with the game's own RmlUi build (UI performance test aid).
// Build: cmd //c "tools\rml_layout_bench.bat"; run: tmp\rml_bench\rml_layout_bench.exe PAGE.rml [PAGE.rml ...]
// For each document: the time to load it, its first layout (styles and layout), and the mean of 20 relayouts forced by
// changing the text of the element with id "desc" (what hovering a row does on a settings page), and its first draw
// with the number of textures that made (each one an upload that waits for the GPU in the game). The context is
// 1920x1080 at dp ratio 1.5, the fonts are assets/InterVariable.ttf, and the render interface draws nothing.
// --dump FILE writes every element's position and size after the first layout, to compare two RmlUi builds' layouts.
// A layout taking tens of milliseconds is a visible stutter when a menu tab opens or the description changes.

#include <RmlUi/Core.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace {
    class NullRender : public Rml::RenderInterface {
    public:
        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override { return 1; }
        void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
        void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
        Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String&) override {
            dimensions = { 1, 1 };
            return 1;
        }
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override {
            generated++;
            return 1;
        }
        void ReleaseTexture(Rml::TextureHandle) override {}
        void EnableScissorRegion(bool) override {}
        void SetScissorRegion(Rml::Rectanglei) override {}

        int generated = 0;   // Textures made (an SVG element's, glyphs'), each an upload in the game.
    };

    class System : public Rml::SystemInterface {
    public:
        double GetElapsedTime() override {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
        bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
            if (type <= Rml::Log::LT_WARNING) {
                std::fprintf(stderr, "rml: %s\n", message.c_str());
            }
            return true;
        }

    private:
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    };

    int count_elements(Rml::Element* e) {
        int n = 1;
        for (int i = 0; i < e->GetNumChildren(); i++) {
            n += count_elements(e->GetChild(i));
        }
        return n;
    }

    void dump_boxes(FILE* f, Rml::Element* e, int depth) {
        Rml::Vector2f pos = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
        std::fprintf(f, "%*s%s %.2f %.2f %.2f %.2f\n", depth, "", e->GetTagName().c_str(), pos.x, pos.y, size.x, size.y);
        for (int i = 0; i < e->GetNumChildren(); i++) {
            dump_boxes(f, e->GetChild(i), depth + 1);
        }
    }

    double ms_since(std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    }
}

int main(int argc, char** argv) {
    FILE* dump = nullptr;
    int first_page = 1;
    if (argc > 2 && std::string(argv[1]) == "--dump") {
        dump = std::fopen(argv[2], "w");
        first_page = 3;
    }
    if (argc <= first_page) {
        std::fprintf(stderr, "usage: rml_layout_bench [--dump FILE] PAGE.rml [PAGE.rml ...]\n");
        return 1;
    }
    NullRender render;
    System system;
    Rml::SetRenderInterface(&render);
    Rml::SetSystemInterface(&system);
    Rml::Initialise();
    if (!Rml::LoadFontFace("assets/InterVariable.ttf", true)) {
        std::fprintf(stderr, "couldn't load assets/InterVariable.ttf (run from the repo root)\n");
        return 1;
    }
    Rml::Context* context = Rml::CreateContext("bench", { 1920, 1080 });
    context->SetDensityIndependentPixelRatio(1.5f);

    for (int i = first_page; i < argc; i++) {
        auto t = std::chrono::steady_clock::now();
        Rml::ElementDocument* doc = context->LoadDocument(argv[i]);
        if (doc == nullptr) {
            std::fprintf(stderr, "couldn't load %s\n", argv[i]);
            continue;
        }
        doc->Show();
        double load = ms_since(t);
        t = std::chrono::steady_clock::now();
        context->Update();
        double first = ms_since(t);
        int textures_before = render.generated;
        t = std::chrono::steady_clock::now();
        context->Render();
        double draw = ms_since(t);
        int textures = render.generated - textures_before;
        if (dump != nullptr) {
            std::fprintf(dump, "# %s\n", argv[i]);
            dump_boxes(dump, doc, 0);
        }

        Rml::Element* desc = doc->GetElementById("desc");
        double relayout = 0.0;
        constexpr int runs = 20;
        if (desc != nullptr) {
            for (int r = 0; r < runs; r++) {
                desc->SetInnerRML(r % 2 ? "Another description of a row." : "A description of a row, long enough to wrap.");
                t = std::chrono::steady_clock::now();
                context->Update();
                relayout += ms_since(t);
            }
            relayout /= runs;
        }
        std::printf("%-36s load %7.2f ms  layout %6.2f ms  relayout %7.2f ms  draw %6.2f ms, %3d textures  (%d elements)\n",
                    argv[i], load, first, relayout, draw, textures, count_elements(doc));
        doc->Close();
        context->Update();
    }
    if (dump != nullptr) {
        std::fclose(dump);
    }
    Rml::Shutdown();
    return 0;
}
