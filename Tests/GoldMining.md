Gold mining regression checks
=============================

Build the normal Final x64 game with Visual Studio/MSBuild. A separate target name
keeps the playable executable intact while testing:

    MSBuild.exe RTEA.sln /t:RTEA /m:2 /p:Configuration=Final /p:Platform=x64 /p:BuildProjectReferences=false /p:TargetName="Cortex Command.mining"

BuildProjectReferences=false assumes the external libraries have already been built.
For a fresh checkout, build the solution normally first.

The terrain-index checks are independent of the engine. In an x64 Developer Command
Prompt, compile and run:

    cl /nologo /std:c++20 /EHsc /O2 /MD /DNOMINMAX /I Source\System Tests\GoldMiningTests.cpp /Fo:build-mp\ /Fe:build-mp\GoldMiningTests.exe
    build-mp\GoldMiningTests.exe

The Lua behavior/scheduler tests require Python and lupa's LuaJIT 2.1 runtime:

    python -m pip install lupa
    python Tests/RunGoldMiningLuaTests.py

`--lupa-dir` supports an isolated dependency directory. These tests load the real AI
scripts with engine fixtures, and do not simulate physics.

Run actual physics/pathfinding acceptance with PowerShell 7.4 or later:

    pwsh -File Tests/RunGoldMiningNativeTests.ps1

The runner loads the fixture module temporarily, uses separate settings, and runs
three digger-only miners in a deterministic scene. Success requires zero remaining
terrain gold, exit code 0 and no AI coroutine errors. It rejects the wrong scene or
an unexpected initial gold count. Runtime logs and console logs show progress; the
completion screenshot is written to ScreenShots. The fixture exits automatically,
and the runner moves its module and runtime log into a dated `build-mp/mining-native-*`
folder, removing the test activity from normal gameplay.

`Tests/GenerateGoldMiningTerrain.py` regenerates the indexed PNG using Pillow. Its
terrain contains shallow/deep gold, distant single-pixel scraps and a solid world
floor. Index tests cover wrapping, map boundary pixels and dynamic scene changes.

The native-test startup switch/environment and fail-fast error logging operate only
when explicitly invoked by the test runner; normal gameplay retains its UI.
