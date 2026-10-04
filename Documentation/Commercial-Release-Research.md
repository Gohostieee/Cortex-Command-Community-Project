# Commercial release of this Cortex Command fork

Research date: October 3, 2026. Checkout examined: `9813e1c12235291f1f82aff25ccb765d27ab8a42`, with an existing local modification to `Source/Managers/MultiplayerMan.cpp`. This is a source-backed feasibility assessment, not clearance of every individual asset or approval from Valve. No publishing, contacting rights holders, or implementation changes were performed.

## Answer

**A paid game can have public source code and be sold on Steam. This particular complete package is not yet cleared for that use.** The engine's AGPLv3 permits charging for copies. Publishing its source satisfies only part of the obligations: it does not resolve inherited asset rights, trademark rights, incompatible proprietary middleware, or Steam contract terms. [AGPLv3 in upstream](https://github.com/cortex-command-community/Cortex-Command-Community-Project/blob/development/LICENSE), [Valve's open-source distribution guidance](https://partner.steamgames.com/doc/sdk/uploading/distributing_opensource?l=english).

There is practical precedent: Mindustry has a public GPLv3 repository and a paid Steam listing. This demonstrates coexistence of public source and sales; its arrangements do not license Cortex Command or authorize this fork's integrations. [Developer's source repository](https://github.com/Anuken/Mindustry), [Mindustry on Steam](https://store.steampowered.com/app/1127400/Mindustry/).

## Rights and compatibility matrix

| Part | Evidence | Commercial assessment | Required action |
| --- | --- | --- | --- |
| Engine and our integrated changes | Root `LICENSE` and `README.md`: AGPLv3 | Charging permitted subject to copyleft and source obligations | Keep covered changes under AGPL; preserve notices; distribute exact matching source and build scripts |
| Original sprites, scenes, UI images/fonts, sound effects and soundtrack | Original DataRealms open-source README excludes game data; current credits retain original contributors | A commercial grant covering every inherited asset was not established | Obtain documented permission from owners with authority, or replace uncleared material |
| Community content and scripts | Merged into `Data`; multiple authors in `Resources/Credits.h` | Repository presence and credits do not establish every contributor's rights | Record each content grant; treat integrated game scripts as covered code unless a valid separate arrangement is established |
| Cortex Command / Data Realms / community branding | `Resources/Credits.h` retains Data Realms trademark and copyright notice | No commercial brand permission established | Obtain permission for retained marks or use an independent identity; retain required historical copyright credits |
| FMOD | Final x64 build links FMOD; current executable imports `fmod.dll` | Proprietary license and AGPL compatibility both unresolved | Replace FMOD, or establish both FMOD distribution rights and valid permission for the engine combination |
| Principal other Windows dependencies | Bundled license files and header notices inspected below | Predominantly commercial-friendly permissive licenses | Assemble accurate third-party notices and exact source for the actual release |
| Visual C++ runtimes / Windows components | Executable import table; Microsoft redistribution documentation | Normal system/runtime distribution requirements | Use approved runtime deployment, preferably Steam Common Redistributables |
| Room/relay service | Own C++ service + bundled RakNet; root project license | Hosting fees are possible; source obligations remain relevant | Offer covered modified network-interactive source under AGPL section 13; retain RakNet notices |
| Steam store distribution | Valve allows open-source applications conditionally; API integration optional | Public source is not itself disqualifying | Resolve rights and contract obligations; propose a standalone DRM-free build |
| Steam achievements, Workshop, matchmaking APIs | Proprietary Steamworks SDK | Additional copyleft compatibility issue | Omit initially, or obtain adequate permissions/review before integration |

Asset evidence and unresolved grants are detailed in [Game-Asset-Rights-Research.md](Game-Asset-Rights-Research.md). Steam source delivery, agreement issues and onboarding are detailed in [Steam-Publishing-Research.md](Steam-Publishing-Research.md).

## What AGPL permits and requires

AGPLv3 sections 4 and 6 permit paid source and executable copies. Section 5 requires covered modifications to remain licensed under AGPL, with notices identifying changes and dates. Buyers retain modification and redistribution rights under section 10. You cannot make inherited AGPL code exclusively proprietary by changing the repository's license, and you cannot prohibit lawful redistribution of the covered work. Public code is not public domain. [Upstream license](https://github.com/cortex-command-community/Cortex-Command-Community-Project/blob/development/LICENSE).

For each shipped release, make its **Corresponding Source** available: your actual modified code, needed build/install scripts, and applicable dependency source. Sections 1 and 6 define the requirements. A visible release-specific source archive/link is preferable to pointing at upstream or a moving development branch. AGPL section 13 also applies to covered modified versions used for remote network interaction. Provide a prominent source offer in the multiplayer flow/service access. These are distribution/network obligations, not a requirement to publish unrelated business systems. [Local license](../LICENSE).

New independent artwork and music you own may be licensed separately when they are genuinely separate works. Do not assume that integrated Lua code, modified inherited assets, or tightly integrated proprietary components qualify as independent merely because they are in a different directory. The license's aggregation provisions and the actual work control. [AGPL section 5](../LICENSE), [FSF aggregation FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#MereAggregation).

## Assets and brand: the largest unresolved permission gap

DataRealms' original open-source repository explicitly excluded game data and directed owners of the commercial game to its Steam beta for compatible data. That original source-code grant is not evidence of a blanket commercial asset grant. The community's later data/source repository merge establishes where files moved, not who had authority to relicense each asset. It also does not prove that no subsequent permission exists. This investigation found no public first-party statement establishing full commercial resale rights for the inherited content. [Original DataRealms README](https://github.com/DataRealms/CCOSS), [archived community data repository](https://github.com/cortex-command-community/Cortex-Command-Community-Project-Data).

The checkout's [credits](../Resources/Credits.h) identify original and community art contributors, Hubnester Industries for intro/menu music, Danny Baranowsky/dB Soundworks for campaign/in-game music, and Michael Watts/Encore Music for “Last Man.” Credits identify people to investigate; they do not prove current ownership or sublicense authority. A commercial agreement should identify covered files and permit modification, commercial redistribution through Steam/Valve, promotional screenshots/trailers, required credits and any necessary sublicense. Music clearance should encompass both recordings and compositions where relevant. A party must have authority to grant those rights; Data Realms permission alone may not cover everything.

The community's historical [sound-replacement planning page](https://github.com/cortex-command-community/Cortex-Command-Community-Project/wiki/Pawnis%27-Sounds-to-be-Replaced-Before-Pre4.0) lists outside sound resources and replacement work. It says most targeted sounds had been replaced, so it cannot establish that any particular current file is unauthorized. It does show why community additions also need per-file provenance rather than a blanket assumption that all sound effects are original and freely sublicensable.

Using a new title helps avoid relying on existing brand permission. It does not authorize copying the existing sprites or music. Conversely, retained attribution should accurately explain the engine's origins without presenting an unofficial product as an official sequel.

## FMOD: two separate questions

The Windows executable inspected imports `fmod.dll`. Its local runtime reports version **2.2.20, build 139317**; included headers are in the `fmod-2.2.13` directory. `Licences/FMOD.TXT` instead describes **FMOD 3.75 (1994–2004)**. That legacy notice is not sufficient evidence of the terms governing the modern runtime. Evidence: [Final x64 build](../RTEA.vcxproj), [Windows dependency declaration](../external/lib/win/meson.build), [legacy notice](../Licences/FMOD.TXT), file version and PE import table inspected October 3.

First, Firelight must authorize distribution. Current public pricing offers Free Indie eligibility below $200,000 annual developer revenue/funding and a project budget below $600,000, with registration and attribution conditions. The next Indie tier is $2,000 per game. Establish which agreement governs the exact runtime and fork; do not assume another developer's license transfers. The current EULA also restricts SDK-file redistribution and distinguishes older FMOD products. [FMOD pricing](https://fmod.com/licensing), [FMOD EULA](https://fmod.com/legal).

Second, an FMOD commercial license does **not** grant permission from the engine's copyright owners to combine their AGPL code with proprietary middleware. No explicit FMOD linking exception was found in the reviewed license/source notices. The FSF's FAQ explains that incompatible non-system library combinations need permission from the relevant program copyright owners. Applying this to the AGPL/FMOD combination is a compatibility concern requiring resolution, not a judicial finding about this project. Dynamic linking alone is not a reliable cure. [FSF incompatible-library guidance](https://www.gnu.org/licenses/gpl-faq.en.html#GPLIncompatibleLibs), [AGPL sections 1, 5, 7 and 12](../LICENSE).

The practical recommendation is to replace FMOD with compatible audio code. Candidates include [miniaudio](https://github.com/mackron/miniaudio) (public domain or MIT No Attribution) or [SDL_mixer](https://github.com/libsdl-org/SDL_mixer) (zlib). Check the exact chosen version and optional decoders. Current SDL3_mixer requires SDL 3.4+, while this checkout bundles SDL 3.2.10, so that choice entails an SDL upgrade. This is real engineering: preserve pitch, spatial audio, streaming/music behavior and performance, then test gameplay sound. It has not been implemented or estimated here.

## Other dependencies examined

This is an inventory of principal declared Windows build dependencies and prominent vendored components, not certification of every file in third-party source trees or optional platform configurations.

| Component | License evidence in checkout | Consequence |
| --- | --- | --- |
| SDL 3.2.10 / SDL_image 3.2.4 | `external/sources/SDL3-3.2.10/LICENSE.txt`; `SDL3_image-3.2.4/LICENSE.txt`: zlib | Commercial use allowed; preserve source notices and mark changes |
| zlib-ng / minizip-ng | Their bundled `LICENSE.md` / `LICENSE`: zlib terms | Commercial use allowed with notice/origin conditions |
| libpng 1.6.40 / loadpng | Bundled licenses: PNG Reference Library License 2 / zlib | Commercial use allowed; preserve notices and mark source changes |
| Allegro 4.4.3.1 custom | `docs/txt/license.txt`: giftware | Broad use/modification/redistribution permission |
| LuaJIT / luabind | `LuaJIT-2.1/src/luajit.h`; `Licences/LICENSE.luabind.txt`: MIT | Include copyright and permission notices |
| RakNet | `external/sources/RakNet/LICENSE`: BSD two-clause | Reproduce copyright, conditions and disclaimer for binary distribution |
| LZ4 | `external/include/LZ4-1.9.3/LZ4/lz4.h`: BSD two-clause | Preserve required source and binary notices |
| Boost 1.75 | Header notices identify Boost Software License 1.0 | Supply appropriate license with source; BSL has an object-code exception for notice inclusion |
| GLM 0.9.9.8 | `copying.txt`: MIT or Happy Bunny; choose MIT | Include MIT notice |
| GLAD loader | Generated header: `(WTFPL OR CC0-1.0) AND Apache-2.0` | Preserve applicable loader/specification notices; do not describe all content merely as MIT |
| Thread pool / hopscotch-map / ImGui | Bundled MIT licenses | Include copyright and permission notices |
| MicroPather | `Source/System/MicroPather/micropather.h`: zlib | Commercial use allowed; preserve notices and mark source changes |
| backward.hpp | Embedded MIT notice | Include notice for included code |
| Tracy / vendored libbacktrace | Bundled BSD three-clause licenses | Preserve applicable source and binary notices |

These local license texts are the primary evidence for the classifications. Boost's official text is available [here](https://www.boost.org/LICENSE_1_0.txt). Optional SDL_image codecs, embedded third-party code and Linux/macOS dependencies need a configuration-specific check before those are packaged. Old license files for codecs in `Licences` are not proof that those codecs are linked into the current Windows executable.

The executable also imports Visual C++ runtimes and standard Windows DLLs. Use the supported [Microsoft redistribution method](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files?view=msvc-170); Steam supports Visual C++ through [Common Redistributables](https://partner.steamgames.com/doc/features/common_redist?l=english). Do not publish toolchain SDK files wholesale.

## Package/source hygiene before a commercial release

[Tools/PackageMultiplayer.ps1](../Tools/PackageMultiplayer.ps1) currently copies `Data`, the legacy `Licences` folder, root license, executable, FMOD and runtime DLLs, and generates a Git commit source URL. It does not assemble a comprehensive notice set from vendored dependencies or bundle an exact source snapshot. Its source URL assumes the shipped binary corresponds to that Git revision; local uncommitted modifications must be captured correctly before shipping. The script is evidence of intended packaging, not proof of legal completeness.

The bundled Windows LZ4 directory contains three headers, while the build links precompiled LZ4 static libraries. This checkout therefore is not itself a complete source bundle for that component. Obtain the matching library source and build information or rebuild from a recorded source release for corresponding-source delivery. Do not infer the binary's exact provenance solely from the header directory's version name.

[Services/RoomService/Package.ps1](../Services/RoomService/Package.ps1) packages service and RakNet source directories but does not list the root license or RakNet license in its archive command. Include the appropriate notices in any release/source package. The relay's source-offer and hosting terms need to preserve AGPL rights for covered software; charging for operating the service is distinct from restricting source rights. No existing packager was changed by this research.

The current game's ordinary PE imports contain no Steam API DLL. The `steam_api.h` reference in [AchievementMan.h](../Source/Managers/AchievementMan.h) is inside a commented-out block, and the Final x64 dependency list has no Steamworks library. This supports planning a standalone Steam build, but is not an exhaustive dynamic-load analysis or Valve approval.

## Recommended routes and practical sequence

**Route A: a permission-based commercial edition.** Approach Data Realms and community maintainers with a concrete list of retained content, title, publishing model and intended changes. Obtain an agreement from parties who own or can sublicense each retained component. Resolve music and contributor gaps, and FMOD/AGPL compatibility. Keep the covered engine open unless all relevant code owners separately grant another license. This preserves existing content but depends on obtaining adequate permissions.

**Route B: an independently branded game using the engine.** Keep the engine and integrated changes under AGPL, create or acquire commercially cleared original assets, replace FMOD, and use a new product identity. Keep an asset manifest and licenses; remove unauthorized mods/content. This reduces dependence on a broad original-IP publishing deal but replacing all uncleared content can be substantial work. It is the more controllable route if grants cannot be established.

For either route:

1. Resolve the asset manifest and product identity before spending on the Steam listing.
2. Remove or lawfully authorize incompatible proprietary combinations; use audited third-party dependencies.
3. Produce a standalone DRM-free executable, matching tagged source/archive, notices and prominent source offers. Keep the current invitation-code multiplayer rather than add Steamworks integration immediately.
4. Present the concrete AGPL/no-runtime-SDK distribution model to Valve. Review the actual Steam Distribution Agreement and recipient terms, especially source delivery and redistribution rights. Do not add an EULA withdrawing AGPL rights. Valve says [API integration is never required](https://partner.steamgames.com/doc/sdk/api?l=english), but warns about [copyleft/SDK combinations](https://partner.steamgames.com/doc/sdk/uploading/distributing_opensource?l=english).
5. Complete identity, tax and bank onboarding; pay the [$100-per-product fee](https://partner.steamgames.com/doc/gettingstarted/appfee?l=english), recoupable at $1,000 Adjusted Gross Revenue. Set price, store materials and accurate features; submit the store/build for review.
6. Plan at least 30 days after fee payment and at least two weeks of Coming Soon. Current Valve pages disagree on 21 versus 30 days for the initial waiting period; confirm the actual dashboard gate. Allow review lead time. [Steam Direct](https://partner.steamgames.com/steamdirect), [onboarding](https://partner.steamgames.com/doc/gettingstarted/onboarding?l=english), [release process](https://partner.steamgames.com/doc/store/releasing?l=english).

Do not build the business around secrecy of the covered executable. Buyers can lawfully share covered copies, subject to the license. A public-source paid release can instead earn its price through maintenance, content, polish, convenient installation, support and services. Profitability was not assessed, and Valve acceptance has not been obtained.
