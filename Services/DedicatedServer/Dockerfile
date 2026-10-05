FROM ubuntu:24.04 AS build
WORKDIR /source
COPY Services/DedicatedServer/InstallDependencies.sh /tmp/cc-install-dependencies
RUN sed -i 's/\r$//' /tmp/cc-install-dependencies && bash /tmp/cc-install-dependencies
COPY . /source
RUN sed -i 's/\r$//' Services/DedicatedServer/Build.sh && bash Services/DedicatedServer/Build.sh /output

FROM scratch AS package
COPY --from=build /output/cc-dedicated-ubuntu24-x86_64.tar.gz /
COPY --from=build /output/cc-dedicated-ubuntu24-x86_64.tar.gz.sha256 /
