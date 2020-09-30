FROM ysucurrency/ysu-env:gcc

ARG NETWORK=live
ARG TRAVIS_TAG=DEV_BUILD
ARG CI_BUILD=OFF
ADD ./ /tmp/src

RUN mkdir /tmp/build && \
    cd /tmp/build && \
    cmake /tmp/src -DCI_BUILD=${CI_BUILD} \
    -DBOOST_ROOT=${BOOST_ROOT} -DPORTABLE=1 \
    -DACTIVE_NETWORK=ysu_${NETWORK}_network \
    -DYSU_POW_SERVER=ON -DYSU_SHARED_BOOST=ON && \
    make ysu_node -j $(nproc) && \
    make ysu_rpc -j $(nproc) && \
    make ysu_pow_server -j $(nproc) && \
    cd .. && \
    echo ${NETWORK} > /etc/ysu-network

FROM ubuntu:18.04

RUN groupadd --gid 1000 ysucurrency && \
    useradd --uid 1000 --gid ysucurrency --shell /bin/bash --create-home ysucurrency

COPY --from=0 /tmp/build/ysu_node /usr/bin
COPY --from=0 /tmp/build/ysu_rpc /usr/bin
COPY --from=0 /tmp/build/ysu_pow_server /usr/bin
COPY --from=0 /tmp/src/api/ /usr/bin/api/
COPY --from=0 /etc/ysu-network /etc
COPY docker/node/entry.sh /usr/bin/entry.sh
COPY docker/node/config /usr/share/ysu/config
COPY --from=0 /tmp/boost/lib/* /usr/local/lib/
RUN chmod +x /usr/bin/entry.sh
RUN ln -s /usr/bin/ysu_node /usr/bin/rai_node
RUN ldconfig

WORKDIR /root
USER root

ENV PATH="${PATH}:/usr/bin"
ENTRYPOINT ["/bin/bash",  "/usr/bin/entry.sh"]
CMD ["ysu_node daemon -l"]
