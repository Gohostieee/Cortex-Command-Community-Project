# Cortex Command game assets and branding: commercial-release research

Research date: October 3, 2026. Scope: the inherited game data and branding in this checkout, using public primary sources. This is evidence for a release decision, not a legal opinion or a completed chain-of-title audit.

## Finding

**The engine's open-source license does not, on the evidence found, establish commercial resale rights to the complete inherited game package.** I found no public first-party instrument expressly granting commercial reuse of all original Cortex Command artwork, audio, text, and branding to arbitrary downstream forks. This is an unresolved permission gap, not proof that the community lacks permission or that a later private grant does not exist.

The original Data Realms release deliberately separated source code from game data. The official [CCOSS repository](https://github.com/DataRealms/CCOSS) describes itself as “no game data included” and directs builders to obtain the data from the Steam beta branch. Its [July 2019 announcement](https://store.steampowered.com/oldnews/?appgroupname=Cortex+Command&appids=209670&feed=steam_community_announcements) announces release of the codebase and links that repository; it does not publish an asset license. These are historical facts about the original release, not sufficient evidence of the terms of every later grant.

The [archived community data repository](https://github.com/cortex-command-community/Cortex-Command-Community-Project-Data) now says its data and source were merged into the unified repository; it had no root LICENSE visible in its repository tree. The [monorepo migration document](https://github.com/cortex-command-community/Cortex-Command-Community-Project/wiki/Migrating-the-Community-Project-to-a-Monorepo) describes preserving history and combining repositories, without documenting an asset relicense. The [current community repository](https://github.com/cortex-command-community/Cortex-Command-Community-Project) calls the project Free/Libre and Open Source under AGPLv3 and includes Data. That is relevant evidence of the maintainers' presentation of the project, but it is not the underlying Data Realms authorization or a file-by-file account of outside rights.

GitHub's own [licensing documentation](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository) explains that public visibility and the ability to view/fork a repository do not replace a copyright license. A downstream release should obtain the missing authorization or use replacements rather than treating public availability alone as commercial clearance.

## What is actually present

The checked-in Data tree at local commit `9813e1c12235291f1f82aff25ccb765d27ab8a42` contains 5,314 PNG files, 2 BMP files, 2,101 FLAC files, 147 OGG files, and 2 XCF files, plus scripts, shaders, configuration and text. These are file counts, not counts of unique works or uncleared works. I did not find a dedicated asset-license or provenance manifest in the local Data tree.

| Content | Evidence | Commercial-release consequence |
| --- | --- | --- |
| Original sprites, scenery, backgrounds, intro art, UI images and promotional art | Local `Resources/Credits.h` credits original artists and retains Data Realms copyright; artist Niklas Jansson's [first-party Cortex Command history](https://androidarts.com/cortexcommand/) describes making much of the graphics and promotional art. | Obtain the original asset grant and its scope; an attribution credit is not a commercial license. Confirm whether Data Realms owns the work or can sublicense it. |
| Original game setting and written content | Existing Data modules and Jansson's [development/lore page](https://androidarts.com/cortexcommand/) document the inherited setting and factions. | New art alone may still leave copied text, expressive designs and setting content. Include these in the rights review or create original content. |
| Intro and menu music | `Resources/Credits.h` credits Robert Stjärnström and Jonas Rörling / Hubnester Industries; `Data/Base.rte/Music.ini` loads `Music/Hubnester/ccintro.ogg` and `ccmenu.ogg`. | Confirm both composition and recording rights, modification and commercial game distribution. |
| Campaign and in-game music | Credits identify Danny Baranowsky / DB Soundworks; Music.ini loads the corresponding OGG recordings. | Confirm sublicense rights for this new product, including trailers and store videos if used there. |
| “Last Man” | Credits identify Michael Watts / Encore Music; Music.ini loads `Music/Watts/Last Man.ogg`. | This is a separately credited recording and must be specifically covered or replaced. |
| Community additions and replacement sounds | Credits identify additional artists/audio contributors. The community's [sound replacement planning document](https://github.com/cortex-command-community/Cortex-Command-Community-Project/wiki/Pawnis%27-Sounds-to-be-Replaced-Before-Pre4.0) names several outside sound-library sources and contributors. | The planning page is provenance evidence, not a per-file license. Determine each shipped asset's source and applicable terms; some library rights may be project-specific or prohibit redistribution of raw files. The page does not establish which current files came from which library. |
| Name and logos | Credits still state “Cortex Command is TM and © 2023 Data Realms, LLC”. | Do not infer permission to brand an independent paid release as official from the code license. Obtain name/logo permission or use a distinct new title and artwork. No trademark-register search was completed. |

The complete original and community credits are available in the [upstream credits file](https://github.com/cortex-command-community/Cortex-Command-Community-Project/blob/development/Resources/Credits.h). They identify potential creators; they do not establish who currently owns copyright, the contracts originally signed, or the scope of any sublicensing powers.

## Two workable routes

### Keep the existing content with written commercial permission

Ask Data Realms and the community maintainers for the actual license or agreement covering the inherited data. If it is not already sufficient, negotiate permission covering the specific release. Get an asset schedule tied to a repository revision or file hashes and confirm:

- worldwide paid distribution on Steam and other stores, plus Valve's necessary distribution rights;
- modifications, ports, patches, multiplayer use and future versions;
- original artwork, animations, UI, text, sounds and every music track, with any exclusions listed;
- rights to the musical compositions and recordings, and marketing/trailer use;
- which community and third-party additions are separately licensed and by whom;
- permission to use the name, logos and original store/marketing art, or agreed independent branding;
- whether raw assets may be included in a public repository and redistributed by users;
- credits, revenue share or fee, term, termination and continued servicing of existing purchasers.

Data Realms could already have the necessary creator agreements, but the credits do not prove that. If it cannot authorize a particular track, voice recording, library sound or community asset, obtain permission from the appropriate owner or replace that item. A grant allowing only free community distribution would not authorize paid resale; I did not locate even that specific grant publicly during this research.

### Build a separately branded game using cleared replacement content

Use the licensed engine and compliant code modifications, give the product a new name and visual identity, and replace inherited assets whose commercial rights cannot be documented. Inventory sprites, terrain, backgrounds, UI icons/fonts, intro slides, faction art, text, music, voices and sound effects. Commission original work under written rights agreements or use assets with explicit licenses supporting the intended distribution. Keep receipts, license versions, author identities and per-file provenance.

Original replacement work can reduce dependency on Data Realms permission, but lightly editing inherited art or audio does not itself resolve the underlying rights. Replacing artwork alone does not clear copied music, names, logos, written content or third-party material. Treat optional mods independently rather than automatically bundling downloaded community content.

## Public contact routes and verification limits

- Data Realms' [official GitHub organization](https://github.com/DataRealms) publicly lists `contact@datarealms.com`. This is a public contact route, not a verified working mailbox. The main site and several historical subdomains were unavailable from this research environment.
- The community's current website [Get Involved source](https://github.com/cortex-command-community/Cortex-Command-Community.github.io/blob/master/src/components/Static/GetInvolved/GetInvolved.js) links [its Discord](https://discord.gg/TSU6StNQUG), and the [repository discussions](https://github.com/cortex-command-community/Cortex-Command-Community-Project/discussions) provide another public route. Ask maintainers to supply the existing grant and explain its downstream scope; cordial developer support is not a substitute for those terms.

No messages were sent. I inspected local data/credits, the original Data Realms repository and Steam announcement, community source/data repositories, issue searches, current discussion listings, public website source and relevant wiki pages. I did not inspect private Discord history, signed contributor contracts, music agreements or a private Data Realms-to-community grant. I found no public primary source that resolves commercial asset licensing. It would therefore be inaccurate to claim either that every inherited asset is definitively AGPL-licensed or that commercial licensing is definitively impossible.
