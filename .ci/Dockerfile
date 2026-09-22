FROM registry.freedesktop.org/drm-hwcomposer/drm-hwcomposer/ubuntu/x86_64_aospless:20260915--aec7a6ce7bb38902c70641526f6611e27141784a

ENV DEBIAN_FRONTEND=noninteractive

ENV PATH="/home/user/bin:${PATH}"

# Taking into account layer structure, everything should be done within one layer.
RUN apt-get update && \
    apt-get install -y clang-19 clang-tidy-19 clang-format-19 git libdrm-dev blueprint-tools libgtest-dev libgmock-dev clang \
    llvm make python3 wget sudo rsync lld pkg-config ninja-build meson \
    python3-mako python3-jinja2 python3-ply python3-yaml

ARG RUN_USER=user
ARG RUN_UID=1000

ENV RUN_USER=${RUN_USER}
ENV RUN_UID=${RUN_UID}
ENV USER_HOME=/home/${RUN_USER}
ENV HOME=${USER_HOME}

RUN mkdir -pv ${USER_HOME}

# Delete default ubuntu user if present
RUN userdel -r ubuntu || true

# Create new user
RUN useradd \
    -m -d ${USER_HOME} \
    -c 'Build User' \
    -s '/usr/bin/bash' \
    -u ${RUN_UID} \
    -o \
    ${RUN_USER} \
    && adduser ${RUN_USER} sudo

RUN cp -r /aospless_arm64 ${USER_HOME}/aospless && \
    rm -rf ${USER_HOME}/aospless/src && \
    ln -s ../drm_hwcomposer/ ${USER_HOME}/aospless/src && \
    chown -R ${RUN_USER}:${RUN_USER} ${USER_HOME} && \
    chmod -R 775 ${USER_HOME}

# Ensure sudo group users are not
# asked for a password when using
# sudo command by ammending sudoers file
RUN echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> \
/etc/sudoers

# Pass control to a newly created user
USER ${RUN_USER}

# Create project path
RUN mkdir -pv ${USER_HOME}/drm_hwcomposer
WORKDIR ${USER_HOME}/drm_hwcomposer

RUN git config --global user.name "FIRST_NAME LAST_NAME" && \
    git config --global user.email "MY_NAME@example.com" && \
    git config --global --add safe.directory '*'

CMD [ "/bin/bash" ]
