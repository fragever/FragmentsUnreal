# FragmentsUnreal

**FragmentsUnreal** is an Unreal Engine plugin for the architecture, engineering,
construction, and operations (AECO) industry. It imports OpenBIM model data in
the [Fragment 2.0](https://github.com/ThatOpen/engine_fragment) `.frag` format
and turns it into Unreal Engine actors, meshes, materials, metadata, and
runtime-streamed spatial representations.

The v1.0 release targets Unreal Engine 5.4 and later.

> 🔄 This plugin is actively developed. Community feedback and contributions are welcome!

---

## 🚀 Features

- 🏗 Import BIM models in Fragment 2.0 format
- 🧱 Translate model hierarchy, geometry, and metadata into Unreal-native assets
- ⚡ Asynchronous loading, shell-first streaming, HISM registration, cell
  merging, proxy meshes, LOD tiers, and optional decimation
- 🔎 Blueprint-accessible metadata, selection, visibility, floor, category, and
  debugging APIs

---

## 📚 Documentation

Online documentation is maintained on the
[FragmentsUnreal documentation site](https://www.fragmentsunreal.dev/docs/).
It includes installation and first-model setup, architecture, guides, and the
complete API reference.


## 📁 Repository Structure

```
FragmentsUnreal/
├── Source/
│   ├── FragmentsUnreal/                   # Core plugin code
|   │   ├── Fragment/                      # Fragment class to store element's data
|   │   ├── Importer/                      # Fragments import logic
|   │   ├── Index/                         # flatbuffers Fragment structure reference
|   │   └── Utils/                         # Functions library
├── ThirdParty/                            # ThirdParty dependencies
│   ├── FlatBuffers/include/flatbufferss   # Flatbuffers structure
│   ├── libtess2                           # libtess2 for mesh triangulation
|   │   ├── include/                       # tesselator header file
|   │   ├── Lib/                           # Libraries for Android/ARM64 and Win64
|   │   └── Source/                        # Source files
├── Content/                               # Assets
│   ├── Materials                          # Base Materials
│   └── Resources                          # Fragments file example
├── FragmentsUnreal.uplugin                 # Plugin descriptor
```

---

## 🧩 Compatibility

* **Unreal Engine**: 5.4 and later
* **Platforms**: Windows and Android (as configured by the current build files)
* **Dependencies**: FlatBuffers (included)
* **Dependencies**: libtess2 (included)

---

## 🤝 Contributing

We welcome pull requests and feature ideas!

### 📌 Contribution Guidelines

* Create a fork or work on a feature branch
* Write clean, commented, and modular code
* Follow Unreal Engine C++ standards
* Add a helpful description to your pull request
* All changes go through `dev` and must be merged via PR

---

## 🛡 License

Original FragmentsUnreal code and documentation are licensed under the [Apache
License 2.0](./LICENSE). Third-party components retain their own licenses; see
[NOTICE](./NOTICE) and the relevant files under `ThirdParty/`.

---

## 🙋‍♂️ Maintainer

**Jairo Andres Basto Picott and Arkoun Merchant**

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution and DCO guidance.

---

## 🌍 Join the Community

If you're integrating Fragment BIM data into Unreal, let’s connect and collaborate! Contributions, issues, or even just stars are appreciated. 🌟

---

## 💖 Support the Project

If FragmentUnreal helps you save development time or adds value to your workflow, consider supporting its development.

Your contribution helps cover maintenance, new features, documentation, and plugin support.

Even a small monthly donation goes a long way!
---
