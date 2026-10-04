# Selling this fork on Steam

Research date: October 3, 2026. Scope: AGPLv3 obligations and publicly accessible English Steam publishing documents. This is a publishing feasibility assessment, not Valve approval or clearance of the game's inherited assets and dependencies.

## Direct answer

**Public source code does not prevent charging for a game.** This checkout's [LICENSE](../LICENSE) contains AGPLv3: section 4 permits charging any price and selling support; section 6 permits charging for executable downloads while providing their corresponding source. Buyers retain the license's modification and redistribution rights. Therefore a paid edition with a public repository is a possible business model, but that fact alone does not establish that this particular package can lawfully ship on Steam. See the [official AGPLv3 text](https://www.gnu.org/licenses/agpl-3.0.html).

## What Valve actually says

Valve's current [English open-source policy](https://partner.steamgames.com/doc/sdk/uploading/distributing_opensource?l=english) says the publisher warrants all necessary distribution rights; incompatible third-party open-source code must not be distributed via Steam. It specifically identifies copyleft licenses as problematic **when combining code with the Steamworks SDK**. It mentions author permission, a different license, or communication that does not invoke copyleft as possible explanations for GPL software already on Steam. Valve says it does not perform codebase legal due diligence.

This is not a blanket statement that all publicly available source is forbidden. Conversely, the presence of other GPL games does not authorize this fork's use of Steamworks. AGPLv3 is a copyleft license; applying Valve's warning to an AGPL/SDK combination is an inference from those sources, not an explicit AGPL-specific approval or rejection.

## The simplest technical route to investigate

Valve explicitly says [Steamworks API integration is never required](https://partner.steamgames.com/doc/sdk/api?l=english). The [SDK overview](https://partner.steamgames.com/doc/sdk?l=english) says its upload tools are required to upload content, while the other SDK features are optional. Using an external upload tool is materially different from linking proprietary Steamworks runtime code into the game.

**Recommended starting proposal:** a standalone executable that also runs outside Steam, no Steamworks runtime linkage, no Steam DRM wrapper, the normal open multiplayer implementation, and complete matching AGPL source releases. The [Steam DRM documentation](https://partner.steamgames.com/doc/features/drm?l=english) describes a wrapper that modifies the executable to verify ownership and launch Steam. Omitting it avoids adding that particular complication; it does not clear every licensing issue.

**Remaining uncertainty:** the public policy does not expressly guarantee acceptance of every unlinked AGPL game. The full Steam Distribution Agreement signed during onboarding was not available for review here. Steam's [Subscriber Agreement](https://store.steampowered.com/subscriber_agreement/?l=english), sections 1.B and 2.G, contains restrictions on copying, modification and commercial use, with exceptions for applicable Subscription Terms. An AGPL game must preserve its recipients' AGPL rights, not impose an ordinary restrictive game EULA on its covered code. Before signing or publishing, obtain Valve's written answer about the proposed AGPL/no-SDK build, how AGPL terms apply to recipients, and source delivery. Review the actual agreement with someone qualified to resolve this specific conflict. Public documentation alone does not settle it.

## What source delivery requires

The following requirements are verified directly against the local [LICENSE](../LICENSE):

- Sections 1 and 6 require **Corresponding Source**, including the actual modifications and needed build/install scripts. A link to upstream's original code is insufficient for a modified release.
- Section 6(d) allows an external source server if equivalent access is provided at no further charge, with clear directions next to the executable download. Keep the release's source available for the required period. A source archive available with the Steam installation plus a visible, version-specific public source link is a practical proposal; its exact Steam presentation still needs confirmation.
- Sections 4 and 5 require preserved notices, a license copy and prominent modification/date notices, and license the covered modified work under AGPL. Interactive legal notices have a specific exception for original interfaces that did not display them.
- Section 10 gives downstream recipients automatic rights and forbids further restrictions. Buyers can redistribute or sell covered copies, subject to AGPL; public source does not mean public domain.
- Section 13 requires a modified version supporting remote network interaction to prominently offer its corresponding source free of charge to those remote users. Assess both the multiplayer game and any covered server components.
- Sections 3 and 12 prevent treating DRM or another agreement as an excuse to withdraw the required freedoms; if conflicting obligations cannot both be met, distribution is not permitted.

Public, tagged source releases are the simplest practical delivery model. AGPL does not indiscriminately require publishing unrelated company software. The FSF's [source-distribution FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#SourceAndBinaryOnDifferentSites) explains matching source and external-server access for the shared GPLv3-style provisions.

## Steam integrations and exceptions

If achievements, Steam matchmaking, Workshop or other API features are wanted later, resolve compatibility **before** adding them. One route is an explicit additional permission or alternative license from every relevant copyright holder; permission for your own changes does not cover other authors' contributions. A proprietary library's commercial license also does not automatically grant an exception to AGPL for the engine code.

A separate launcher or helper process is not an automatic loophole. Section 5 permits independent aggregation; the FSF's [aggregation explanation](https://www.gnu.org/licenses/gpl-faq.en.html#MereAggregation) evaluates both communication mechanism and meaning, and says intimate exchange of internal structures may still produce one combined work. The proposed architecture would require a concrete review.

## Publishing steps after rights clearance

1. Finish the separate asset, brand, engine and dependency rights audit; choose cleared content and a lawful product identity.
2. Prepare the standalone release, matching source archive, notices and version-specific source links. Resolve middleware incompatibilities independently of Steam.
3. Present the proposed distribution model to Valve, resolve the agreement/source questions, then onboard with the proper legal name, identity, bank and tax information. [Onboarding](https://partner.steamgames.com/doc/gettingstarted/onboarding?l=english) allows an individual to onboard; forming a corporation is not mandatory.
4. Pay the [Steam Direct fee](https://partner.steamgames.com/doc/gettingstarted/appfee?l=english): $100 USD per product, plus applicable taxes. It is nonrefundable but recoupable after $1,000 Adjusted Gross Revenue.
5. Build the store page, pricing and reviewed build. Valve's [release process](https://partner.steamgames.com/doc/store/releasing?l=english) requires both store and build approval; allow 3–5 business days for store review and submit at least seven days ahead. Coming Soon must be public for at least two weeks.
6. Confirm the actual release gate in the dashboard. **Current public sources conflict:** [onboarding documentation](https://partner.steamgames.com/doc/gettingstarted/onboarding?l=english) says 21 days after fee payment; [Steam Direct's signup page](https://partner.steamgames.com/steamdirect) says 30 days. Budget at least 30 days and verify with Valve rather than promise a 21-day launch.

The commercial proposition is a convenient maintained release, updates, polish, support and potentially separately operated services. Public code and authorized redistribution make exclusive control of the covered executable a poor foundation for the business. The Steam route is plausible to pursue after rights clearance and agreement review; the existing package has not been cleared merely by publishing its source.
