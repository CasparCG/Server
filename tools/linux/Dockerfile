# Build CasparCG and bundle its shared libraries into a separate runtime image.
FROM docker.io/buildpack-deps:noble AS build-casparcg
	ADD tools/linux/install-dependencies /
	ADD tools/linux/platform-config /
	ADD tools/linux/check-compiler /
	ADD tools/linux/install-cef /

	ARG CC
	ARG CXX
	ARG GIT_HASH

	# Download CEF during the build so its runtime can be bundled in the final image.
	RUN /install-dependencies --download-cef

	RUN mkdir /source && mkdir /build && mkdir /install

	WORKDIR /build

	RUN wget https://sdk.lunarg.com/sdk/download/1.4.328.1/linux/vulkansdk-linux-x86_64-1.4.328.1.tar.xz
	RUN tar -xf vulkansdk-linux-x86_64-1.4.328.1.tar.xz && \
		mv 1.4.328.1 /tmp/vulkan-sdk
	ENV VULKAN_SDK=/tmp/vulkan-sdk/x86_64
	ENV PATH=$VULKAN_SDK/bin:$PATH

	COPY ./src /source

	RUN cmake -GNinja /source -DUSE_STATIC_BOOST=ON -DUSE_SYSTEM_CEF=OFF -DENABLE_VULKAN=ON

	RUN cmake --build .

	RUN cmake --install . --prefix staging

	# Collect linked libraries alongside the staged server for the runtime image.
	RUN ln -s /build/staging /staging && \
		/source/shell/copy_deps.sh /build/shell/casparcg /staging/lib

FROM docker.io/ubuntu:noble
	COPY --from=build-casparcg /staging /opt/casparcg

	RUN set -ex; \
			apt-get update; \
			DEBIAN_FRONTEND="noninteractive" apt-get install -y --no-install-recommends \
			tzdata \
			libc++1 \
			libnss3 \
			fontconfig \
			libglvnd0 \
			libgl1 \
			libglx0 \
			libegl1 \
			libgles2 \
			; \
			rm -rf /var/lib/apt/lists/*

	WORKDIR /opt/casparcg
	RUN chmod 755 run.sh
	ADD tools/linux/run_docker.sh ./
	CMD ["./run_docker.sh"]
