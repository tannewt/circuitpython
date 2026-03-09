FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-pip python3-venv python3-dev \
    git git-lfs \
    build-essential cmake mtools uncrustify \
    gettext wget curl ca-certificates \
    pkg-config autoconf automake libtool \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/python python /usr/bin/python3 1

# dosfstools 4.2 (newer than apt's 4.1)
RUN wget -q https://github.com/dosfstools/dosfstools/releases/download/v4.2/dosfstools-4.2.tar.gz \
    && tar xf dosfstools-4.2.tar.gz \
    && cd dosfstools-4.2 && ./configure && make && make install \
    && cd .. && rm -rf dosfstools-4.2*

COPY requirements-dev.txt /tmp/
RUN pip install --break-system-packages -r /tmp/requirements-dev.txt \
    && rm /tmp/requirements-dev.txt

WORKDIR /circuitpython
