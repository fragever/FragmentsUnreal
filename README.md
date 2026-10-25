# FragmentsUnreal

**FragmentsUnreal** is an Unreal Engine plugin for the architecture, engineering,
construction, and operations (AECO) industry. It loads BIM models in That Open
Company's [Fragments 2.0](https://github.com/ThatOpen/engine_fragment) `.frag`
format and turns them into Unreal actors, instanced meshes, materials, and
queryable metadata, streamed at runtime with level of detail built during import.

Targets Unreal Engine 5.4 and later. No Nanite required.

> 🔄 This plugin is actively developed. Community feedback and contributions are welcome!

---

## 🚀 Features

**Loading and performance**

- 🏗 Load `.frag` models in the editor or at runtime from Blueprint (`LoadFragmentAsync`)
- ⚡ Whole buildings stream in, shell and interior together. File loading and mesh
  building run on background threads, and the rest of the import is paced against
  the frame budget
- 🔁 Repeated elements (windows, columns, fixtures) become instanced meshes
- 🔭 Continuous, view-driven level of detail: each model moves between a simplified
  proxy (LOD2), merged cells (LOD1) and individual elements (LOD0) by how much of
  the screen it covers. No clicks or activation needed
- 🌑 One shadow caster per model, and a memory budget for merged cells
  (`r.Fragments.Memory.CellBudgetMB`)

**BIM data and visualization**

- 🔎 Metadata queries from Blueprint: categories, attributes, property sets,
  element info and model metadata
- 🧱 Floor visibility ranges, category filters, ghosting and x-ray views
- 🎯 Element selection at any distance: `SelectAtScreenPos` promotes the model
  through its LOD tiers until it finds the element under the cursor
- ✏️ Selection outline material and a material parameter collection (`MPC_Fragments`)
  for ghost settings

**Editor**

- 🗂 A **Fragments Viewer** panel (Window → Fragments Viewer): batch import with
  cancel, a model list that follows the level, material overrides with
  drag-and-drop, live placement fields, and undo/redo
- 🎥 Imported models stream through the same LOD path in the editor, driven by the
  viewport cameras (`r.Fragments.EditorStreaming`)
- 🐞 One debug view for every LOD tier (`r.Fragments.LODDebug.Mode`)

---

## 🔄 Getting a `.frag` file

FragmentsUnreal reads `.frag`, not `.ifc`. To convert an IFC model:

- **[FragmentsUnreal Viewer](https://www.fragmentsunreal.dev/viewer/)**: open your
  `.ifc` in the browser, check it, and download the `.frag`. The file is processed
  in your browser and never uploaded.
- **[ifc2frag](https://github.com/fragever/ifc2frag)**: the viewer's source, if you
  want to run it locally (`npm install`, then `npm run dev`).
- Or use That Open's Fragments tooling directly: see
  [docs.thatopen.com](https://docs.thatopen.com/).

---

## ⚙️ Quick Start

You need Unreal Engine 5.4 or later on Windows, Visual Studio with the
**Game development with C++** workload, and a **C++ project**. The plugin ships as
source, so your project has to build it. If your project is Blueprint-only, add
one C++ class first (Tools → New C++ Class) so Unreal creates a build target.

1. Clone or download this repository and copy it to
   `[YourProject]/Plugins/FragmentsUnreal/` (the `.uplugin` file sits directly in
   that folder).
2. Close the editor, right-click your `.uproject`, choose **Generate Visual
   Studio project files**, and build **Development Editor / Win64**.
3. Open the project and enable **FragmentsUnreal** in Edit → Plugins.
4. Open **Window → Fragments Viewer** in the editor, pick a `.frag` file, and
   click **Load Fragment**.

The [getting started guide](https://www.fragmentsunreal.dev/docs/getting-started)
walks through each step with screenshots, plus the Blueprint runtime path.

---

## 📚 Documentation

The [FragmentsUnreal documentation site](https://www.fragmentsunreal.dev/docs/)
covers installation, loading models in the editor and at runtime, the load
sequence, materials, rendering with Movie Render Queue, Blueprint examples, the
architecture, and the complete API reference.

---

## 📁 Repository Structure

```
FragmentsUnreal/
├── Source/
│   ├── FragmentsUnreal/          # Runtime module
│   │   ├── Fragment/             # AFragment actor: per-element data, floors, visibility
│   │   ├── Importer/             # Import pipeline and Blueprint subsystem
│   │   ├── Spatial/              # Cells, proxies, LOD tiers and screen-space error
│   │   ├── Selection/            # Screen-position selection and traces
│   │   ├── Index/                # FlatBuffers Fragments schema
│   │   ├── Debug/                # LOD debug view
│   │   └── Utils/                # Frame budget, classification, helpers
│   └── FragmentsEditor/          # Editor module: Fragments Viewer panel, overrides
├── ThirdParty/
│   ├── FlatBuffers/              # FlatBuffers headers
│   └── libtess2/                 # Polygon triangulation (Win64 and Android ARM64 libs)
├── Content/
│   ├── Materials/                # Base, glass, outline and debug materials, MPC_Fragments
│   └── Resources/                # Panel fonts and icons
└── FragmentsUnreal.uplugin       # Plugin descriptor
```

---

## 🧩 Compatibility

* **Unreal Engine**: 5.4 and later
* **Platforms**: Windows (Win64). libtess2 libraries for Android ARM64 are included
* **Engine plugins**: ProceduralMeshComponent and GeometryScripting (enabled by the plugin)
* **Third-party**: FlatBuffers and libtess2 (included)

---

## 🐛 Issues

Found a bug or have a feature idea? Open an
[issue](https://github.com/fragever/FragmentsUnreal/issues) with your Unreal Engine
version, platform, and steps to reproduce.

---

## 🤝 Contributing

We welcome pull requests and feature ideas!

* Fork the repository and work on a feature branch
* Open your pull request against `main` with a clear description
* Follow the Unreal Engine C++ coding standard
* Sign off your commits (see the DCO section in [CONTRIBUTING.md](CONTRIBUTING.md))

---

## 🛡 License

Original FragmentsUnreal code and documentation are licensed under the [Apache
License 2.0](./LICENSE). Third-party components retain their own licenses; see
[NOTICE](./NOTICE) and the relevant files under `ThirdParty/`.

---

## 🙋‍♂️ Maintainers

**Jairo Andres Basto Picott and Arkoun Merchant**

---

## 🌍 Join the Community

If you're bringing BIM data into Unreal, let's connect! Issues, pull requests,
and stars are all appreciated. 🌟 And if you build something with FragmentsUnreal,
we'd love to see it.
