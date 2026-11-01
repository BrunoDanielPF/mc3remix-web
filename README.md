# Midnight Club 3: DUB Edition Remix no navegador

Objetivo: executar a edição **PlayStation 2 USA** de *Midnight Club 3: DUB Edition Remix* no navegador por recompilação estática, em uma máquina local.

## Cópia identificada

A imagem presente em Downloads foi identificada por leitura do formato ISO 9660, sem execução do jogo:

- Volume: `MC3REMIX`
- Executável de boot: `SLUS_213.55`
- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_213.55;1`, `VMODE = NTSC`
- Tamanho: 8.539.963.392 bytes
- SHA-256: `DE199F7E7F57B7CB603A129217F7482D50644FD66558450DFEB5612857B732F9`

Essa é uma imagem de **PS2 Remix**. O plano anterior, baseado no `default.xbe` de Xbox do `mcdub3-nx`, foi substituído. O [plano técnico](docs/PORTING_PLAN.md) documenta a nova base, a prova local e os próximos critérios de aceitação.

## Base do porte

O projeto [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) transforma o executável R5900 de uma ISO de PS2 em C++ e WebAssembly, com runtime de PS2 e WebGPU. Revisão examinada: `333999564ad0fe84f24b504416869dc182404a61`. O [nfsu2-web](https://github.com/th0mk/nfsu2-web) continua como referência de arquitetura e validação para jogos recompilados no navegador.

O código fonte da base, sob licença MIT, está em [`engine/`](engine/). A ISO, o código gerado do jogo e os builds ficam fora do Git. O servidor de teste deve escutar apenas em `127.0.0.1`, pois disponibiliza os dados da ISO a quem conseguir acessá-lo. O [ponto de retomada dos testes de FPS](docs/PERFORMANCE_CHECKPOINT.md), o [relatório completo](docs/PERFORMANCE.md), o [plano do caminho gráfico direto](docs/GS_RENDER_FAST_PATH.md), o [plano de multiplayer](docs/MULTIPLAYER_PLAN.md) e o [mapa de recursos visuais e menus](docs/UI_ASSETS.md) registram o estado técnico.

## Versionamento

O repositório público contém código fonte e documentação. `main` registra a base inicial; `develop` é a linha de trabalho para as próximas mudanças, com commits específicos por alteração. A ISO, cartões de memória, estados salvos, código recompilado a partir do jogo, dependências e builds permanecem locais e ignorados pelo Git. A licença MIT da base está em [`engine/LICENSE`](engine/LICENSE); uma licença para o projeto como um todo ainda não foi definida.

## Estado

O pipeline reconheceu a ISO, recompilou o executável e concluiu os builds nativo e WebAssembly. No navegador, percorri a abertura, a tela inicial, o aviso de cartão de memória, o menu principal e entrei em **Arcade → San Diego → Cruise**. A cidade, o carro e o painel foram renderizados. No Cruise controlado, a Intel Gen12-LP exibiu cerca de **3,1–3,4 FPS**, enquanto o Edge com NVIDIA Ampere exibiu **57,22 FPS** em 120 quadros. Duas instâncias também largaram e avançaram na mesma corrida LAN **Ordered Race — Rites of Passage**; a chegada e o resultado final continuam pendentes. Áudio e salvamento de uma sessão longa ainda não foram validados. Evidências e próximos passos: [plano técnico](docs/PORTING_PLAN.md), [plano multiplayer](docs/MULTIPLAYER_PLAN.md) e [ponto de retomada de FPS](docs/PERFORMANCE_CHECKPOINT.md).

O alvo é **60 FPS exibidos**, com **30 FPS como mínimo jogável**. O estado salvo no Cruise mediu cerca de 3,1 FPS exibidos no navegador integrado. O perfil encontrou cerca de 1.575 dispatches WebGPU por imagem. Um protótipo opcional de fusão de uploads pequenos e paletas reduziu 28,7% dos dispatches em 81 quadros e elevou o FPS exibido de 3,11/3,13 para 3,34/3,34 em duas comparações; quatro capturas nativas ficaram idênticas. A opção continua desligada por padrão. O grafo de dependências agora registra VRAM, CLUT, textura decodificada e scratch para orientar um caminho mais amplo de renderização. Método, limites e próximos passos: [desempenho](docs/PERFORMANCE.md).

O pré-carregamento seletivo da ISO antes do Start também foi testado. Com 13,75 MiB dos blocos usados no Cruise carregados em memória, todas as 41 buscas do runtime na janela medida foram atendidas sem HTTP, mas o FPS exibido permaneceu em **3,30**, contra **3,27** sem pré-carga. A opção experimental `disc-preload=<manifesto.json>` pode ajudar o streaming em rede; o caminho gráfico do GS continua sendo o gargalo para atingir 30–60 FPS. Resultados e formato do manifesto: [desempenho](docs/PERFORMANCE.md).

O transporte multiplayer inicial já cria salas e encaminha pacotes binários entre abas com um relay WebSocket independente. A [página de teste e o protocolo](docs/MULTIPLAYER_PLAN.md#camada-implementada-salas-e-relay) permitem validar essa camada sem alterar a sessão do jogo. Em um build diagnóstico separado, duas instâncias abriram os menus originais, descobriram a hospedagem e entraram no mesmo **LOBBY** automaticamente. Em 01/11/2026, ambas largaram na mesma **Ordered Race**, exibiram posições **1/2** e **2/2**, e avançaram na pista com o carro remoto visível. Ainda faltam validar a chegada e o resultado, corridas de carreira/Arcade, identidades no menu original e uma taxa de quadros jogável com dois clientes.

A página do jogo agora permite escolher **Jogar solo** ou **Multiplayer** antes do Start. Multiplayer cria ou entra em uma sala, mostra os participantes e copia um convite. No build de diagnóstico, a sala já encaminha os quadros Ethernet ao SMAP emulado, e os jogadores usam os menus LAN originais para entrar no mesmo lobby. O [estado e limite dessa integração](docs/MULTIPLAYER_PLAN.md#seleção-solo--multiplayer-na-página-do-jogo) registra os modos e validações pendentes.

O [mapeamento da interface original](docs/UI_ASSETS.md) encontrou os menus e fundos de rede dentro de `ASSETS.DAT`. Um teste isolado trocou um rótulo da tabela de textos e o jogo exibiu o novo texto no menu, preservando a arte original e a ISO. O menu **NETWORKING** está acessível com sondas opt-in no build diagnóstico; a entrada continua dependente dessas sondas e da implementação de rede emulada.

Para reproduzir o build, use Linux ou WSL e uma cópia própria da ISO. Mantenha os dados do jogo e os arquivos gerados fora do repositório. A fonte versionada está em `engine/`; copie-a para um diretório de build separado. Consulte também as [dependências e instruções da base](engine/README.md).

```bash
mkdir -p "$HOME/mc3remix-build"
cp -a engine/. "$HOME/mc3remix-build/"
cd "$HOME/mc3remix-build"
scripts/port.sh mc3remix "/caminho/para/sua-copia/MC3-Remix.iso" --frames 4200 --rounds 4
python3 scripts/serve.py build/web-mc3remix games/mc3remix/disc.iso
```

Abra a [prévia local com a introdução avançada](http://127.0.0.1:8620/?press=300:start,600:cross,900:cross,1200:cross,1500:cross) em um navegador com WebGPU e clique em **Start**. O servidor de teste escuta apenas em `127.0.0.1` porque entrega bytes da ISO local.

Para reconstruir após editar `engine/`, sincronize o código fonte com o checkout WSL e rode o build web:

```bash
cp -a engine/. "$HOME/mc3remix-build/"
cd "$HOME/mc3remix-build"
scripts/build_web.sh mc3remix
```

Em uma instalação nova, `scripts/port.sh` gera o código do jogo e prepara os dois builds localmente. A ISO, o código gerado e os binários compilados não são distribuídos neste repositório.

Controles web para corrida: `W` acelera (X do PS2), `S` freia/dá ré (□), `A`/`D` esterçam pelo analógico esquerdo, `Espaço` ou `Q` acionam o freio de mão (R1), e `E` ou `2` acionam o nitro (R2). `Shift` é L1 (transferência de peso), `R` ou `1` é L2 (olhar para trás), `C` muda a câmera (△), `V` é L3 (manobra especial), `X` é ○ (faróis/aceitar corrida). `I`/`J`/`K`/`L` também controlam o analógico esquerdo. `N` ou `Backspace` é Select; `Enter` é Start/pausa. As setas são o direcional do PS2 para menus, mapa, modo de corrida e música; `Z` também é X do PS2 para confirmar. `F` alterna tela cheia. O mapeamento segue os controles padrão do [manual do Remix](https://www.videogamemanual.com/PS2/Midnight%20Club%203-%20DUB%20Edition%20Remix%20%28GH%29%20%28USA%29.pdf).

Para avançar a abertura, pressione `Z` algumas vezes durante os logotipos. No aviso de cartão, use seta para baixo e `Z` em **Continue Without Saving**. No menu principal, selecione **Arcade**; as opções padrão levam ao modo Cruise. A abertura sem essas entradas pode permanecer visualmente no último quadro do vídeo; o runner nativo apresenta o mesmo comportamento.
