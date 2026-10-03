FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake g++ make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY external/sources/RakNet external/sources/RakNet
COPY Source/System/MultiplayerProtocol.h Source/System/MultiplayerWorldProtocol.h Source/System/MultiplayerRelay.h Source/System/MultiplayerTransport.* Source/System/
COPY Tests/MultiplayerRelayTests.cpp Tests/MultiplayerRelayTests.cpp
COPY Services/RoomService Services/RoomService
RUN cmake -S Services/RoomService -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build -j 2
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libstdc++6 && rm -rf /var/lib/apt/lists/* && useradd --uid 10001 --create-home room
COPY --from=build /build/cc-room-service /usr/local/bin/cc-room-service
USER 10001:10001
EXPOSE 8001/udp
ENTRYPOINT ["cc-room-service"]
CMD ["--port", "8001", "--max-rooms", "100", "--max-mbps", "800"]
