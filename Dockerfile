FROM ubuntu

RUN apt-get update && apt-get install gcc make git bison flex -y
RUN git clone https://github.com/google/nsjail.git
RUN cd /nsjail && git submodule update --init && cd kafel && make && cd .. && make
RUN mv /nsjail/nsjail /bin && rm -rf -- /nsjail

