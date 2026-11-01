# Medição de desempenho do porte

Atualizado em 27/10/2026. Esta página registra o próximo ciclo técnico para *Midnight Club 3: DUB Edition Remix* no navegador. A fonte recompilada é a edição PS2 `SLUS_213.55`.

## Estado da recompilação

O `ps2-web-recomp` já traduz estaticamente o código EE/R5900 do jogo para C++ e depois para WebAssembly. Os microprogramas VU1 descobertos também são compilados. Assim, recomeçar com um elevador de instruções no estilo do `nfsu2-web` só ajudaria se um perfil mostrar custo dominante no código EE ou VU. O `nfsu2-web` recompila a versão PC x86, cujo sistema operacional e API gráfica são diferentes dos da edição PS2.

## Instrumentação web

A prévia local aceita `bench-from=<quadro>` e `bench-frames=<quantidade>` na URL. Ao terminar a janela, o console emite uma única linha `PS2_BENCHMARK` em JSON. Ela contém FPS de apresentações, FPS efetivamente exibido pela página, percentis 50 e 95 de ambos os intervalos, estimativa de tempo GPU por quadro e tempo ocioso das threads EE, VU e GS. O limite é 10.000 quadros. Sem `bench-frames`, a instrumentação não coleta amostras. O campo de GPU é extrapolado pelo runtime a partir de consultas de timestamp e pode superar o tempo de parede; não deve ser interpretado como ocupação real da GPU.

`frames=<quadro absoluto>` encerra uma execução finita e imprime no console os dispatches acumulados por pipeline. `PS2_GPU_TIMESTAMPS=0` desativa apenas as consultas de timestamp para comparação. `PS2_GPU_UPLOAD_DIAG=1` ativa, no runner nativo, o diagnóstico de assinaturas de upload e de páginas de VRAM invalidadas; não altera o resultado renderizado. Essas opções são de investigação e não representam otimizações aprovadas.

Exemplo para uma cena carregada por estado salvo local:

```text
http://127.0.0.1:8620/?state=perf-4100.state&bench-from=4101&bench-frames=120
```

O arquivo `perf-4100.state` usado neste experimento fica fora do Git, em `games/mc3remix/`. O estado foi capturado na tela de nome de perfil, portanto mede um menu, não a direção em San Diego. O mesmo estado, resolução, navegador e quantidade de quadros devem ser usados em cada comparação. Uma medição isolada da abertura ou do menu não representa o Cruise ou uma corrida.

## Primeiros achados

- Na prova manual anterior, a abertura atingiu aproximadamente 60 FPS, os menus ficaram em 10–17 FPS e o Cruise em 5–8 FPS no navegador integrado da máquina. São observações, não uma comparação controlada.
- A revisão upstream `79d8eab` foi compilada em checkout separado. Ela inicializou e renderizou a tela de perfis. Nessa tela, indicou cerca de 8–12 FPS, sem ganho evidente frente à ordem de grandeza observada antes. O teste ainda não colocou as duas revisões no mesmo estado salvo.
- Um perfil nativo de 300 quadros na tela de nome de perfil gerou 4.950 amostras, sem perdas. Aproximadamente 3.166 foram atribuídas a `libc.so.6`, 1.179 a `pthread_cond_broadcast` e 69 a `libvulkan_lvp.so`. Esse perfil do runner Linux não localiza o gargalo do WebGPU no navegador; serve apenas para mostrar que a amostragem nativa precisa de símbolos/threads mais específicos.
- A correção upstream `b837b2e` para o cache de uploads do GS foi avaliada. Quando a entrada mais antiga ainda é lida por um envio pendente, ela envia esse trabalho e tenta inserir a textura novamente. O upstream relatou melhora no GT4. No MC3, a amostra do menu abaixo mostrou piora, então a correção não foi mantida na base principal.

## Comparação A/B no mesmo estado

No navegador integrado visível, adaptador `intel gen-12lp`, resolução interna 640×512, ambos os builds carregaram `perf-4100.state` e mediram 120 apresentações a partir do quadro 4101. A única diferença de código foi a correção `b837b2e` no cache do GS. Foram feitas duas execuções de cada versão, alternando base/correção/correção/base e fechando a aba anterior antes da seguinte.

| Versão | FPS de apresentações (2 execuções) | P95 entre apresentações | Estimativa GPU por quadro |
| --- | --- | --- | --- |
| Base | 9,53; 9,31 | 241,4; 250,0 ms | 64,8; 105,3 ms |
| Cache corrigido | 8,94; 9,09 | 412,1; 338,3 ms | 2.358,7; 2.252,0 ms |

A versão corrigida ficou cerca de 4–6% mais lenta nesse menu, com pior cauda de latência. A estimativa de GPU da versão corrigida excede o tempo de parede por quadro e não é uma medida confiável de utilização; é provável que a extrapolação do runtime seja inadequada quando o patch muda a quantidade de envios. Esta janela curta não mostra o efeito no Cruise, mas não justifica aplicar o patch como otimização geral.

O FPS da tabela conta callbacks de quadros produzidos pelo runtime, não necessariamente quadros distintos vistos na tela. O contador separado `onScreenFps` foi adicionado depois dessa comparação para medir o ritmo de apresentação via `requestAnimationFrame`; ainda não há amostras pareadas desse campo.

A instrumentação nova foi validada em outra janela visível de 60 apresentações da versão base: 9,08 FPS produzidos, 8,93 FPS exibidos e P95 de 257 ms entre quadros exibidos. A imagem do estado salvo continuou correta. Essa amostra curta verifica o campo novo; não deve ser comparada diretamente à janela A/B de 120 apresentações.

## Cruise reproduzível

O runner nativo percorreu a partir do estado de perfil `perf-4100.state` estas entradas, sempre em quadros absolutos: `4120:triangle`, `4200:up`, `4260:cross` para sair do perfil; `4620:down`, `4660:down`, `4700:cross` para Arcade; `5180:cross` para opções; `5430:cross` para Start; `5730:cross` para entrar no Cruise. O carro e o HUD apareceram no quadro 6000. O estado `route-6300.state` foi salvo já na rua de San Diego e carregou corretamente no navegador. Esses estados, a ISO e as capturas permanecem somente no WSL local, fora do Git.

O botão `cross` acelera o carro nesse modo; `r2` não o moveu no teste. A URL local abaixo reproduz uma janela curta de movimento a partir do estado salvo:

```text
http://127.0.0.1:8620/?state=cruise-6300.state&press=6310:cross:220&bench-from=6350&bench-frames=120
```

Na primeira janela, iniciada logo após carregar o estado (quadros 6302–6423), o navegador integrado visível produziu 7,32 FPS e exibiu 4,33 FPS; o P95 entre quadros exibidos foi 485,8 ms. A imagem confirmou deslocamento do Eclipse até a parede da rua.

Uma segunda janela descartou os primeiros 50 quadros de aquecimento e mediu os quadros 6350–6470. Ela produziu 6,53 FPS e exibiu 3,16 FPS, com P95 visível de 602,8 ms. A nova instrumentação do loop gráfico mediu, por quadro produzido:

| Etapa do loop GS | Tempo médio |
| --- | ---: |
| Trabalho dentro de `render_pending`, excluindo a espera de apresentação | 51,9 ms |
| Espera de apresentação via `requestAnimationFrame`/`MessageChannel` | 100,2 ms |
| Espera por novos comandos | 0,0 ms |
| Ceder ao navegador via `browser_yield` | 0,7 ms |

O tempo total foi 153,0 ms por quadro produzido nessa janela. A espera de apresentação respondeu por cerca de dois terços desse tempo; o trabalho do GS respondeu por cerca de um terço. Os tempos são medidos no thread de renderização e não identificam, sozinhos, se a espera vem da fila WebGPU, do agendamento da página ou do compositor.

Testei esperar pela apresentação a cada dois quadros, mantendo o resto igual. O resultado foi 6,76 FPS produzidos, 2,93 FPS exibidos e P95 visível de 618 ms. O trabalho medido do GS subiu para 62,5 ms por quadro. A mudança não melhorou a experiência e foi retirada; a fonte principal continua esperando a cada quadro.

## Perfil do GS em Cruise

O perfil nativo `PS2_GPU_PROFILE=6350,6360,...` registrou cinco imagens apresentadas (quadros 6351, 6353, 6355, 6357 e 6359). Cada uma exigiu aproximadamente **1.575 dispatches WebGPU**, com pouca variação:

| Pipeline | Dispatches por imagem, aproximadamente |
| --- | ---: |
| Upload | 681 |
| Carga de CLUT | 332 |
| Rasterização | 229 |
| Pré-decodificação de textura | 273 |
| Snapshot de VRAM | 63 |

Dos 3.405 dispatches de upload nas cinco imagens, 2.585 tinham no máximo quatro workgroups. Todas as cargas de CLUT tinham um workgroup. Os tempos de timestamp do perfil nativo vêm do adaptador Lavapipe e **não** representam o custo absoluto no Intel WebGPU do navegador; a sequência e as contagens de comandos são o dado útil aqui.

O script `engine/scripts/gpu_topology.py` calcula o teto de uma fusão simples: substituir cada sequência contígua de uploads pequenos e cargas de CLUT por um único dispatch, sem atravessar rasterização, snapshot ou decodificação. Se o limite for quatro workgroups por upload, a economia máxima é **585 de 1.578 dispatches por imagem**; cerca de 993 ainda restariam. Mesmo permitindo todos os uploads, restariam aproximadamente 720. Isso é só uma contagem teórica, não uma previsão de FPS, e mostra que a fusão de pré-operações sozinha não alcança a redução de tempo necessária. A estratégia precisa tratar também rasterizações, decodificações, snapshots e sincronização, preservando um caminho genérico para estados GS que o caminho rápido ainda não represente.

No fluxo do quadro 6351, os 1.588 comandos se dividem em 681 uploads, 332 cargas de CLUT, 281 decodificações, 231 rasterizações e 63 snapshots. Só **10 pares de rasterizações são adjacentes**; fundir apenas rasterizações vizinhas economizaria no máximo 10 dispatches nesse quadro. Em 120 rasterizações, há uma carga de CLUT imediatamente antes e um upload imediatamente depois; outras 61 aparecem entre snapshots. Um caminho rápido útil precisa tratar o conjunto de operações e seus buffers intermediários, não apenas reunir comandos `raster` contíguos.

Um perfil nativo separado do mesmo quadro contou 11.209 primitivas nas 231 rasterizações; **11.124 (99,2%) usam textura**. Só dois lotes, com 16 primitivas ao todo, são inteiramente sem textura. Os 63 lotes que pediram snapshot tinham uma primitiva cada. Os outros **168 lotes somam 11.146 primitivas**, sendo 11.061 texturizadas. A mediana foi 12 tiles por lote, mas os dez maiores concentraram 51,5% dos tiles. Tempos por pipeline desse perfil vêm do Lavapipe e não devem ser projetados no WebGPU do cliente. Esses dados descartam um caminho rápido limitado a primitivas sem textura ou à fusão de rasterizações adjacentes. Um protótipo útil deve começar pelo desenho texturizado sem feedback; os lotes que precisam de snapshot permanecem no renderizador genérico até haver uma implementação correta.

## Protótipo de reordenação segura

`PS2_REORDER_PRE=1` ativa um experimento no `emit_pre`: um upload atravessa somente cargas de CLUT anteriores cujas faixas de VRAM não se sobrepõem à sua escrita. A ordem relativa dos uploads e a ordem relativa das cargas de CLUT são preservadas. A opção permanece **desligada por padrão**.

Na execução nativa de 80 quadros desde `route-6300.state`, o protótipo moveu 23.263 pares de operações e reduziu os dispatches de upload de 27.714 para 27.034 e os de CLUT de 14.426 para 13.431. Rasterização (13.749), snapshot (6.104) e decodificação (11.588) não mudaram. As capturas nos quadros 6320, 6340, 6360 e 6380 ficaram idênticas byte a byte.

No navegador integrado, duas janelas alternadas com o mesmo estado, entrada e resolução deram:

| Versão | FPS produzidos | FPS exibidos |
| --- | --- | --- |
| Base | 6,60; 6,82 | 3,30; 3,07 |
| Reordenação | 6,63; 6,31 | 3,48; 3,26 |

O FPS exibido subiu pouco nas duas amostras, enquanto o produzido caiu na segunda. Essa quantidade de dados não sustenta habilitar o protótipo como padrão, e a economia de dispatches fica longe da exigida para 30 FPS. A opção serve para validar a análise de dependências e para experimentos futuros.

## Fusão experimental de uploads e paletas

`PS2_FUSE_PRE=1` usa um único workgroup de 256 threads para executar, na ordem original, até 16 uploads pequenos (no máximo 256 pixels cada) e cargas de CLUT. Barreiras de armazenamento separam as operações. Uploads maiores e os demais pipelines mantêm o caminho anterior. A opção permanece desligada por padrão.

Em 81 quadros nativos desde `route-6300.state`, com X pressionado, quatro capturas nos quadros 6320, 6340, 6360 e 6380 ficaram idênticas byte a byte. Os dispatches totais caíram de 76.371 para 54.426 (**−28,7%**). Rasterização, snapshot e decodificação não mudaram. O build web concluiu.

No navegador integrado, com `intel gen-12lp`, 640×512 e a janela de 120 apresentações no Cruise, a execução alternada base/fusão/base/fusão resultou em:

| Versão | FPS produzidos | FPS exibidos |
| --- | --- | --- |
| Base | 6,43; 6,47 | 3,11; 3,13 |
| Fusão | 6,78; 6,58 | 3,34; 3,34 |

O ganho exibido foi de cerca de 7% neste trecho. As janelas da fusão começaram no quadro 6351 e as da base no 6350, por uma diferença de agendamento ao carregar o mesmo estado; isso limita a precisão da comparação. O resultado não justifica ativar a opção como padrão antes de validar outras cenas e dispositivos. Mesmo neste caso favorável, o FPS ficou muito abaixo do mínimo de 30.

## Grafo de acesso à VRAM

O runtime aceita `PS2_GPU_ACCESS_PROFILE=<início>,<fim>,<arquivo>` no runner nativo. Para cada comando, exporta faixas conservadoras de VRAM, CLUT, textura decodificada e scratch lidas e escritas. `engine/scripts/gpu_access_graph.py` converte as faixas em páginas de 8 KiB para VRAM (512 páginas, com retorno circular), 2 KiB para CLUT e 4 KiB para os buffers temporários. Ele gera arestas de leitura após escrita (`RAW`), escrita após leitura (`WAR`) e escrita após escrita (`WAW`). O formato anterior, de sete colunas só com VRAM, ainda é aceito. O JSON é um artefato local em `games/mc3remix/`, fora do Git.

Com o estado 6300 e aceleração pelo X, o quadro 6351 gerou **1.588 comandos e 5.569 pares** de dependência no grafo original só de VRAM. A reordenação experimental reduziu esse quadro para 1.545 comandos e 5.509 pares. O novo rastreio dos mesmos comandos encontrou **6.845 pares** ao incluir os buffers auxiliares. Entre as relações adicionais estão 725 CLUT RAW, 207 textura decodificada RAW e 63 scratch RAW. O raster usa esses resultados intermediários, então movê-lo apenas pela ausência de conflito de VRAM seria incorreto. Cópia, resolução e exibição também têm acessos anotados; um quadro de exibição isolado foi verificado. As faixas são conservadoras e transferências da CPU, submissões e estados não modelados ainda exigem manter a ordem original. O grafo orienta a investigação, mas não autoriza sozinho fundir comandos arbitrários.

Reprodução local:

```bash
PS2_GPU_ACCESS_PROFILE=6350,6353,games/mc3remix/access-multi.txt \
  build/native-mc3remix/ps2run games/mc3remix/disc/SLUS_213.55 \
  --load-state games/mc3remix/route-6300.state --frames 6360 --press 6310:cross:50
python3 scripts/gpu_access_graph.py games/mc3remix/access-multi.txt \
  --frame 6351 --json games/mc3remix/access-graph-multi-6351.json
```

Uma execução finita no navegador integrado até o quadro 6480 contabilizou 61.919 uploads, 36.498 cargas de CLUT, 51.962 rasterizações, 31.143 snapshots e 28.215 decodificações em 205 envios. Isso inclui trabalho desde a restauração do estado e não deve ser dividido somente pelos 120 quadros da janela de benchmark.

## Comparações adicionais no mesmo Cruise

Todas as amostras abaixo usaram `route-6300.state`, entrada `6310:cross:220`, janela de 120 quadros a partir de 6350 e resolução interna 640×512, exceto o teste 1280×1024. Cada linha é uma execução, portanto diferenças pequenas ainda exigiriam repetição antes de virar padrão.

| Navegador/opção | FPS produzidos | FPS exibidos | P95 exibido |
| --- | ---: | ---: | ---: |
| Integrado, base | 6,54 | 3,16 | 604 ms |
| Integrado, `PS2_DECODE=0` | 5,94 | 2,77 | 687 ms |
| Integrado, escala 2 | 3,56 | 1,87 | 625 ms |
| Edge, base | 6,21 | 3,42 | 389 ms |
| Edge, `PS2_GPU_TIMESTAMPS=0` | 6,28 | 3,03 | 659 ms |

Desativar a pré-decodificação retirou os 28 mil dispatches `decode` da execução finita, mas piorou o FPS exibido. Desativar as consultas de timestamp também não mostrou ganho. Dobrar cada dimensão da resolução quase reduziu à metade o FPS. A correção upstream de cache `b837b2e` produziu 6,40 FPS no Cruise frente a 6,53 FPS da base em uma comparação curta; não foi incorporada.

No runner nativo, 80 quadros após o estado 6300 geraram 30.052 uploads. Destino, formato, posição e bytes se repetiram em 24.774 deles (82,4%). Depois de invalidar conservadoramente as páginas de VRAM tocadas por uploads, rasterizações e cópias locais, **apenas 8** repetiram com todas essas páginas intactas. Portanto um cache por página que descarta uploads idênticos quase não ajudaria. O rastreio por páginas pode rejeitar casos seguros em pixels diferentes da mesma página; qualquer tentativa mais fina precisará de prova de correção visual.

Com o alvo declarado pelo usuário, **30 FPS exibidos são o mínimo de jogabilidade e 60 FPS é a meta**. A base de 3,16 FPS exibidos precisa reduzir o tempo entre imagens em cerca de 9,5 vezes para 30 FPS e 19 vezes para 60 FPS. Ajustes isolados de espera, resolução, consulta de timestamp, cache ou fusão de pré-operações não têm magnitude suficiente. O próximo passo é usar o grafo multirrecurso para desenhar um caminho de renderização mais amplo, que reduza também rasterizações, decodificações, snapshots e sincronizações.

## Próximo experimento

1. Prototipar um lote inteiro com textura e profundidade em pipeline gráfico WebGPU, isolado por opção e comparado por pixel ao renderer compute. A triagem mostrou que lotes sem Z não cobrem o Cruise.
2. Criar o espelho de textura/VRAM e dividir lotes mistos sem mudar a ordem GS, seguindo [caminho gráfico direto](GS_RENDER_FAST_PATH.md), com fallback compute para feedback e modos especiais.
3. Comparar capturas, FPS exibido e percentis em Cruise e em uma corrida antes de habilitar o novo caminho. `PS2_FUSE_PRE=1`, `PS2_CHUNK_WRITES=1` e `PS2_DECODE_CACHE=1` continuam opções reversíveis de ganho pequeno.
4. Medir o custo de apresentação em uma GPU dedicada com o navegador em primeiro plano. O teste anterior da RTX 3050 em Edge apresentou quadros a cerca de 1 Hz em segundo plano e não serve para comparar GPUs.
5. Considerar outro pipeline de recompilação após medir o custo do EE. A referência do `nfsu2-web` usa x86/Win32/D3D9 e não substitui a tradução do GS do PS2.

O critério para promover uma otimização é melhora repetível no mesmo cenário, sem regressão visual, de entrada, áudio ou salvamento.

## Diagnóstico de envio e caminho gráfico direto

`PS2_GPU_CPU_DIAG=1` separa, a cada 20 submissões, criação de buffers, gravação da arena e dos parâmetros, codificação, submissão e eventos. No navegador integrado com GPU `intel gen-12lp`, a arena teve 1,5–2,3 MiB e os parâmetros 0,2–0,33 MiB por submissão. Em várias janelas sem pausas, as gravações e a codificação ficaram cada uma abaixo de 1 ms por submissão. Outras janelas registraram dezenas de ms concentrados em diferentes chamadas; esses tempos de parede incluem pausas do navegador e não demonstram uma operação GPU persistentemente lenta. No runner nativo com `llvmpipe`, `queueSubmit` ficou perto de 342 ms por submissão: é rasterização por software e não representa o navegador com GPU física.

`PS2_CHUNK_WRITES=1` divide as gravações da arena e dos parâmetros em partes de até 1 MiB. Quatro capturas nativas (6320, 6340, 6360 e 6380) foram idênticas byte a byte com a opção desligada e ligada. No mesmo Cruise, duas execuções sem blocos exibiram **3,21 e 3,11 FPS**, e duas com blocos exibiram **3,36 e 3,36 FPS**. Uma execução com blocos e `PS2_FUSE_PRE=1` exibiu **3,33 FPS**. A pequena melhora dos blocos não se somou à fusão; ambas as opções permanecem experimentais. As janelas de 120 quadros começaram em 6350 ou 6351, conforme o agendamento do estado salvo.

`scripts/gpu_graph_depth.py` analisou o grafo multirrecurso do quadro 6351: **1.588 comandos, 6.845 pares de risco e 1.387 níveis dependentes**. Considerando apenas a VRAM, ainda são **1.087 níveis**. Esse limite é otimista e usa páginas de 8 KiB, que podem introduzir dependências falsas; ele também não modela todas as transferências da CPU. Mesmo assim, a cadeia longa mostra por que reordenar os mesmos dispatches não deve fornecer o ganho de quase 10× necessário. O perfil por primitiva encontrou **346.710 de 521.500** dentro de uma seleção ampla para um futuro pipeline gráfico, incluindo os modos de alfa e mistura comuns. Critérios e implementação incremental: [caminho gráfico direto](GS_RENDER_FAST_PATH.md).

## Cache de textura e limites dos lotes

`PS2_GPU_TEXTURE_DIAG=1` mede reaproveitamento de decodificações sem mudar o renderer. Em 81 quadros nativos de Cruise, 1.421 das 11.905 decodificações elegíveis repetiram a mesma configuração com páginas de VRAM e slot de CLUT inalterados. Elas somaram 67,3 milhões dos 172,0 milhões de texels candidatos; é um limite superior sem orçamento de memória. `PS2_DECODE_CACHE=1` implementa reaproveitamento **somente dentro da submissão WebGPU**, usando chaves completas, versões conservadoras de páginas e CLUT, e offsets que não são sobrescritos até o envio. Os dispatches de decode caíram de 11.905 para 11.529 (−3,2%). Quatro capturas completas de Cruise (6320, 6340, 6360, 6380) e três de menu (4120, 4140, 4160) tiveram hashes idênticos com a opção desligada e ligada. No navegador integrado, com apenas uma aba do jogo por vez, a janela base marcou **6,53 FPS produzidos / 3,27 exibidos** e a janela com cache **6,80 / 3,40**. É uma única comparação de pequena diferença; o cache permanece desligado por padrão. Uma comparação anterior com duas abas simultâneas foi descartada por contenção de GPU.

Uma cache persistente experimental com mais 16 MiB lotou em 41 de 48 submissões e elevou as decodificações a 13.995. A variante foi retirada. O grafo do quadro 6351 mostrou 231 comandos raster: só 10 pares eram adjacentes; em 207 dos 230 intervalos havia comandos intermediários exigidos por dependências, deixando apenas 23 pares com movimentação possível no modelo otimista. A seleção ampla de 346.710 primitivas também se distribui por lotes mistos: apenas 3.768 de 14.538 lotes inteiros, contendo 97.004 primitivas, atendem aos critérios. **Nenhum desses lotes dispensa teste de profundidade.** O pipeline gráfico precisa modelar Z e dividir lotes mistos para alcançar cobertura relevante; uma passagem gráfica limitada a lotes opacos sem Z teria cobertura nula neste trecho.

Dois microexperimentos adicionais foram descartados após comparação isolada no navegador integrado. Antecipar o teste da caixa da primitiva antes de carregar seu estado preservou quatro capturas byte a byte, mas caiu de **3,29 para 2,88 FPS exibidos** no mesmo Cruise; o shader original foi restaurado. Pré-calcular na CPU os formatos de textura, framebuffer e Z também preservou quatro capturas, mas marcou **3,31 FPS exibidos ligado e 3,33 desligado** no mesmo build, com mais dados por lote; o protótipo foi retirado. Essas medições isoladas não demonstram ganho e não justificam elevar o custo do caminho padrão.

Um novo `PS2_GPU_FASTPATH_TRACE=<início>,<fim>,<arquivo.csv>` registra somente metadados de lotes. `scripts/gpu_fastpath_trace.py` mostra que os dois quadros de desenho 6350 e 6352 tiveram 130 lotes inteiramente elegíveis e texturizados, com 3.985 de 22.440 primitivas. Os grupos uniformes mais úteis para o primeiro pipeline usam PSMT8 ou PSMT4, framebuffer PSMCT32 e profundidade PSMZ16S. A seleção detalhada está em [caminho gráfico direto](GS_RENDER_FAST_PATH.md).

`PS2_FASTPATH_SPLIT=1` divide, em caráter experimental, a sequência ao alternar entre primitivas elegíveis e inelegíveis, mantendo o renderer compute. Quatro capturas do Cruise permaneceram byte a byte iguais. Em 81 quadros nativos, os lotes inteiros elegíveis passaram de 97.004 para 346.710 primitivas, mas os dispatches raster subiram de 14.538 para 24.222 e os de CLUT de 14.882 para 23.610; os de decode caíram de 11.905 para 5.467. No navegador integrado, uma janela isolada marcou **2,99 FPS exibidos com divisão**, frente a **3,32 sem divisão** no mesmo build. A opção fica desligada por padrão. Para ser útil, a passagem gráfica terá de substituir os novos lotes compute e superar esse custo medido; uma única comparação ainda não quantifica com precisão sua magnitude.

O teste de outras cenas manteve essa decisão. No menu de perfil, três capturas com divisão foram idênticas à base; o navegador exibiu **4,35 FPS com divisão e 4,34 na base** em uma janela isolada por versão, enquanto os FPS produzidos caíram de 9,83 para 7,67. Na transição de entrada no Cruise, outras quatro capturas foram idênticas. O rastreio de lotes e a análise de modos foram ampliados em [caminho gráfico direto](GS_RENDER_FAST_PATH.md); a divisão é uma ferramenta de preparação, não uma otimização para ativar agora.

A contagem de quadros produzidos vem de `Module.onFrame` após `render_pending`; a de quadros visíveis é amostrada por `requestAnimationFrame` na página. No Cruise base isolado, foram 6,53 FPS produzidos e 3,27 FPS amostrados na tela, com 119,1 ms por quadro em trabalho GS e 33,0 ms em espera de apresentação. A leitura é que parte das apresentações é substituída antes do próximo ciclo visual do navegador sob carga; ela não indica 2× de quadros recuperáveis apenas mudando a contagem ou removendo a espera. Reduzir o custo gráfico continua sendo necessário.

## Pré-carregamento do conteúdo do disco

O host web lê a ISO por requisições HTTP Range de 256 KiB. O cache de 256 entradas comporta até 64 MiB e já antecipa blocos sequenciais. Para testar a hipótese de que a leitura do disco limita o FPS, acrescentei contadores de buscas, bytes e verificações de bloco ainda pendente. `PS2_DISC_TRACE=1` registra `PS2_DISC_FETCH frame=N chunk=N`; `disc-preload=<manifesto.json>` carrega uma lista de blocos **antes de habilitar Start**. O manifesto tem `discSize`, `chunkBytes: 262144` e `chunks` (índices dos blocos), aceita até 256 índices e precisa corresponder exatamente ao tamanho da ISO. O navegador mantém os bytes em memória e atende as leituras do runtime a partir dela; uma falha de pré-carga volta ao fluxo sob demanda. A ISO e os bytes pré-carregados continuam locais, fora do Git.

Medi o mesmo estado `cruise-6300.state`, entrada `6310:cross:220`, quadros 6350–6470, 640×512, GPU Intel Gen12-LP e `PS2_CHUNK_WRITES=1`, usando uma aba por vez. O manifesto inicial veio do rastreio da própria cena; uma segunda rodada acrescentou os cinco blocos que faltavam. Cada linha é uma execução curta, sujeita a variação do navegador.

| Blocos pré-carregados | Dados antes de Start | Buscas do runtime na janela | Atendidas pela pré-carga | Verificações pendentes | FPS produzidos | FPS exibidos | Trabalho GS/quadro |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 40 | 0 | 931 | 6,54 | 3,27 | 122,6 ms |
| 50 | 12,5 MiB | 41 | 36 | 163 | 6,56 | 3,34 | 116,3 ms |
| 55 | 13,75 MiB | 41 | 41 | 223 | 6,50 | 3,30 | 121,0 ms |

Na última linha, **todas as 41 buscas do runtime foram atendidas pela memória pré-carregada**, sem nova requisição HTTP dentro da janela. Ainda houve verificações de blocos pendentes porque o runtime instala os blocos no cache por um serviço assíncrono; esse contador mede tentativas, não milissegundos de espera. A pré-carga dos 55 blocos levou 248 ms no servidor local. A cidade, o carro e o HUD continuaram corretos na inspeção visual. Mesmo nesse limite favorável, o FPS exibido ficou perto de 3,3. Portanto pré-carregar dados pode reduzir esperas de streaming e melhorar a estabilidade quando a ISO estiver em rede, mas **não resolve o gargalo principal dessa cena**. Também aumenta o tráfego inicial e a memória; pré-carregar a ISO inteira (7,95 GiB) não é apropriado para o navegador ou para uma entrega via VPS.

Para reproduzir localmente, coloque um manifesto junto ao build servido e use, por exemplo, `?state=cruise-6300.state&press=6310:cross:220&bench-from=6350&bench-frames=120&PS2_CHUNK_WRITES=1&disc-preload=preload-cruise.json`. O manifesto experimental está somente em `build/web-mc3remix/preload-cruise.json` no WSL e contém índices, não conteúdo da ISO. Em um VPS, a pré-carga deve ser opcional e limitada a trechos escolhidos; ainda faltam medições de latência e banda reais desse ambiente. O próximo trabalho de FPS continua sendo reduzir as muitas operações e dependências do GS com o caminho gráfico direto, conferindo capturas para evitar regressão visual.

## Apresentação e custo por operação no WebGPU

No mesmo Cruise, mesma aba, Intel Gen12-LP, 640×512 e janela de 120 quadros, comparei a apresentação normal com `nosurface=1`. A segunda opção ainda processa os comandos GS, mas não desenha no canvas; seu contador de FPS exibidos não representa imagens visíveis. Uma execução normal produziu **6,59 FPS** e exibiu **3,35 FPS**; sem superfície, produziu **6,95 FPS**. A diferença de cerca de 5% nos quadros produzidos é insuficiente para alcançar 30 FPS. Uma execução com `PS2_FUSE_PRE=1` e `PS2_DECODE_CACHE=1`, além de `PS2_CHUNK_WRITES=1`, produziu **6,46 FPS** e exibiu **3,45 FPS**. A inspeção da imagem não encontrou regressão óbvia, mas essa diferença pequena em uma amostra não autoriza ligar as opções por padrão nem substitui comparação por pixel.

O modo `PS2_GPU_PROFILE=<início>,<fim>,<arquivo>` passou a registrar no console `PS2_GPU_PROFILE_SUMMARY` por tipo de operação e `PS2_GPU_PROFILE_TOP` para os cinco lotes raster mais caros. Corrigi o diagnóstico para perfilar também comandos enviados junto com a apresentação; o modo padrão de renderização não muda. O profiler separa **cada comando em uma passagem compute e espera o readback**, por isso os tempos abaixo não são a duração de um quadro normal ou uma previsão de FPS. Servem para localizar trabalho caro na GPU do próprio navegador, em vez de depender dos tempos do Lavapipe nativo.

| Cena e quadro perfilado | Comandos | Raster | Decode | CLUT | Upload | Cinco rasterizações mais caras |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cruise 6351 | 1.586 | 227 em 179,5 ms | 282 em 23,7 ms | 332 em 18,9 ms | 681 em 17,6 ms | 74,2 ms |
| Menu de perfil 4103 | 1.403 | 148 em 154,5 ms | 374 em 30,0 ms | 290 em 19,8 ms | 590 em 14,5 ms | 78,5 ms |

Nos dois quadros, a soma medida das rasterizações respondeu por cerca de 70–74% dos tempos de operações do perfil. Os lotes mais caros cobrem 847–1.024 tiles de 16×16 pixels; no Cruise, os cinco primeiros representam cerca de 41% do tempo raster medido, e no menu, 51%. As capturas das duas cenas continuaram visualmente corretas no teste. A próxima implementação deve priorizar esses lotes grandes e texturizados, com teste de profundidade e equivalência visual, antes de tentar otimizar apenas as numerosas operações pequenas. O perfil precisa ser repetido após o primeiro protótipo gráfico, pois dividir passagens e readback acrescenta custo ao diagnóstico.

## Variante de 64 threads por tile

Testei uma variante opcional do kernel raster com 64 threads por tile de 16×16 pixels: cada thread calculou quatro pixels em sequência, em vez das 256 threads da base. Mantive a mesma lista de primitivas, ordem por pixel e resolução. No runner nativo, as capturas dos quadros 6310, 6320, 6330 e 6340 foram **idênticas byte a byte** entre as versões. No navegador integrado, no mesmo build, cena e janela de 120 quadros, a variante produziu **4,92 FPS e exibiu 2,50 FPS**, contra **6,62 produzidos e 3,37 exibidos** na base. O trabalho GS por quadro subiu de 107,5 para 161,5 ms. A variante piorou o FPS exibido em cerca de 26% e foi retirada do código; não altera o caminho padrão.

Os lotes mais caros do quadro 6351 são em grande parte mistos. Um lote de 104 primitivas cobriu 896 tiles; outro de 624 primitivas cobriu 896 tiles e teve mais de 13 mil entradas nas listas de tiles no rastreio nativo correspondente. Reduzir o número de threads por tile não remove o laço por primitiva executado em cada pixel. Um caminho gráfico direto precisará processar esses lotes grandes, inclusive ao dividir modos mistos, para ter alcance sobre o custo medido.

## Shader especializado para sprites e comparação com nfsu2-web

`PS2_GPU_PRIM_TRACE=<quadro>,<comando>,<arquivo.csv>` registra os estados das primitivas de um lote para investigação. Um dos lotes raster caros contém dois sprites grandes, texturizados, com teste Z sempre aprovado e escrita Z desligada. Experimentei `PS2_SPRITE_FAST=1`: quando todas as primitivas do lote são sprites e ele cobre pelo menos 128 tiles, o runtime usa um kernel compute que contém apenas o cálculo de sprite. Quatro capturas nativas da rota até o Cruise ficaram **idênticas byte a byte** às da base; o caminho padrão não usa a opção.

No navegador integrado, 640×512, Intel Gen12-LP, `PS2_CHUNK_WRITES=1` e a mesma janela de 120 quadros do Cruise, a base exibiu **3,39 e 3,32 FPS** em duas execuções; o shader de sprites exibiu **3,44 e 3,46 FPS**. A melhora observada é pequena (cerca de 0,1 FPS) e insuficiente para o objetivo de 30–60 FPS. Os tempos de trabalho GS não caíram de modo consistente; não ativar por padrão nem extrapolar esta amostra para outras cenas.

O [projeto nfsu2-web](https://github.com/th0mk/nfsu2-web) recompila o executável x86 de PC para C/Wasm e traduz comandos Direct3D 9 para WebGPU. Seu [documento de arquitetura](https://github.com/th0mk/nfsu2-web/blob/main/docs/web/DESIGN.md#3-the-renderer-contract-c-recorder--typescript-webgpu-encoder) descreve um gravador de quadros e recursos em memória compartilhada, uploads agrupados, cache de pipelines e bind groups, e renderização por passagens gráficas. O nosso código EE/VU já é recompilado para Wasm; a diferença decisiva no trecho medido é o GS do PS2, cuja VRAM, CLUT, blend e feedback ainda são processados por muitos dispatches compute. Usar o mesmo princípio requer criar uma tradução própria de desenhos GS comuns para render passes WebGPU e sincronizá-los com a VRAM, conforme [plano do caminho gráfico](GS_RENDER_FAST_PATH.md). O wrapper Direct3D 9 do nfsu2-web não processa comandos GS diretamente.

A documentação do nfsu2-web também descreve pré-carregamento opcional e cache OPFS para arquivos, útil para streaming em VPS. Nosso teste de pré-carga eliminou as buscas HTTP dentro da janela medida sem elevar o FPS exibido de forma significativa. Portanto esse recurso pode ajudar latência de carga, mas o primeiro marco de desempenho continua sendo migrar os lotes raster grandes para passagens gráficas, com comparação de imagem e fallback compute para modos não equivalentes.

## Limite experimental sem rasterização

Acrescentei `PS2_GPU_SKIP_RASTER=1` como diagnóstico, desligado por padrão. Ele deixa a fila GS, uploads, CLUT, decodificação, apresentação e emulação funcionando, mas **não despacha os kernels raster**. A imagem fica incorreta; este modo não é uma otimização nem validação visual. Seu objetivo é medir o custo residual da arquitetura atual antes de construir o caminho gráfico.

No navegador integrado com Intel Gen12-LP, 640×512, estado `cruise-6300.state`, entrada `6310:cross:220`, `PS2_CHUNK_WRITES=1` e 120 quadros a partir de 6350:

| Modo | FPS produzidos | FPS amostrados na tela | Trabalho GS/quadro |
| --- | ---: | ---: | ---: |
| Base no mesmo build | 6,57 | 3,34 | 110,6 ms |
| Sem raster, execução 1 | 22,70 | 11,35 | 29,5 ms |
| Sem raster, execução 2 | 22,23 | 11,49 | 34,8 ms |
| Sem raster + `PS2_FUSE_PRE=1` | 24,14 | 11,27 | 35,1 ms |

Os números mostram que o raster é o maior custo individual. Mesmo sua eliminação completa **não alcançou 30 FPS produzidos nem exibidos** neste navegador e nesta cena. A fusão das pré-operações elevou a produção para 24,14, mas não aumentou o FPS amostrado na tela. Não se deve interpretar 11,35–11,49 como um teto matemático para um renderer novo: passagens gráficas agrupadas podem também remover uploads, decodificações, barreiras e apresentações redundantes. O teste estabelece que simplesmente substituir kernels raster um a um, mantendo o restante, não será suficiente para a meta. O próximo protótipo deve medir o ciclo completo de lote gráfico + coerência VRAM e investigar a cadência da apresentação.

Acrescentei `rafCallbacks` e `rafRateHz` ao benchmark. Com raster normal, uma execução teve **61 callbacks e 61 quadros amostrados** em 18,62 s: ambos a **3,28 Hz**, contra 6,45 quadros/s produzidos. Sem raster, houve **60 callbacks e 60 quadros amostrados** em 5,13 s: ambos a **11,69 Hz**, contra 23,39 quadros/s produzidos. O contador de tela não está deixando de marcar callbacks que ocorreram; a cadência do `requestAnimationFrame` na página é que cai sob a carga desta execução. Isso ainda não separa uma limitação de composição do navegador integrado de uma característica do nosso agendamento de GPU. A primeira tentativa no Edge com NVIDIA ocorreu com a janela minimizada e foi descartada; a medição válida posterior está em [comparação de adaptadores](#comparação-de-adaptadores-e-alvo-de-otimização).

## Prova de render pass para os lotes GS

`PS2_RASTER_PASS=<quadro>,<comando>` ou `PS2_RASTER_PASS=all` ativa uma variante experimental. Ela desenha um quad por tile em um render pass WebGPU; o fragment shader percorre a mesma lista de primitivas e usa a mesma função `shade_at`/`plot` do kernel compute para escrever na VRAM. O attachment de cor é apenas um alvo para a rasterização; **textura, framebuffer e Z ainda não foram migrados para attachments nativos**. A opção fica desligada por padrão.

O lote selecionado no runner nativo foi o comando 1076 do quadro 6350, com **77 primitivas e 122 tiles**. Sua execução passou sem erros de validação WebGPU. Na variante `all`, as seis capturas nativas dos quadros **6310, 6320, 6330, 6340, 6350 e 6360** foram idênticas byte a byte à base. No navegador integrado, o modo `all` mostrou cidade, carro e HUD sem erro WebGPU aparente. A comparação de FPS usou o mesmo build, 640×512, Intel Gen12-LP, Cruise 6350–6470 e `PS2_CHUNK_WRITES=1`:

| Modo | FPS produzidos | FPS amostrados na tela | Trabalho GS/quadro |
| --- | ---: | ---: | ---: |
| Compute base | 6,61 | 3,25 | 126,0 ms |
| Render pass por lote, execução 1 | 5,41 | 2,66 | 169,7 ms |
| Render pass por lote, execução 2 | 5,43 | 2,63 | 168,3 ms |

A variante de um pass por lote **regrediu aproximadamente 18% em FPS** nesta cena. Ela mantém o laço de pixels e alterna compute/render muitas vezes; essa medição não testa ainda o desenho direto de primitivas com color/depth attachments e cache de texturas do plano. O diagnóstico de tempo GPU por timestamp foi desligado nessa variante porque o intervalo original cobria somente uma passagem compute; o valor `gpuEstimateMsPerFrame: 0` não deve ser usado. O próximo teste de desempenho deve transformar os vértices GS em draw calls dentro de um render pass agrupado e sincronizar a VRAM apenas nas fronteiras reais de leitura, mantendo o fallback compute.

## Draw instanciado por primitiva

O protótipo `PS2_PRIMITIVE_PASS=all` emite um quad por primitiva para lotes PSMT8 uniformes. Na cena Cruise até o quadro 6353, ele substituiu **43 lotes e 835 primitivas**, e 53 capturas nativas consecutivas foram iguais à base. O hash da VRAM logo após o primeiro lote amplo foi idêntico entre draw e compute; omitir esses lotes alterou o quadro 6353. Esta seleção representa 3,7% das 22.440 primitivas do rastreio de três quadros.

Benchmark no navegador integrado, Intel Gen12-LP, 640×512, estado `cruise-6300.state`, entrada `6310:cross:220`, janela de 120 quadros e `PS2_CHUNK_WRITES=1`:

| Modo | FPS produzidos | FPS exibidos | Trabalho GS/quadro |
| --- | ---: | ---: | ---: |
| Compute base, mesmo build | 6,58 | 3,40 | 109,0 ms |
| Draw por primitiva, lotes elegíveis | 6,47 | 3,18 | 125,5 ms |
| Draw por primitiva, logs reduzidos | 6,61 | 3,14 | 131,3 ms |

As janelas começaram nos quadros 6351 e 6350, respectivamente, por variação do agendamento de apresentação. A terceira medição suprimiu o registro de cada lote no console para evitar esse custo; o caminho normal não foi alterado. As pequenas diferenças de produção entre execuções não demonstram ganho, e o FPS exibido continuou perto de 3. O draw ainda chama a rotina GS de escrita pixel a pixel na VRAM. O próximo experimento precisa usar attachments nativos de cor e Z e manter sua coerência com a VRAM em grupos de lotes. O valor de GPU por timestamp é zero no modo experimental porque a consulta atual não abrange passagens mistas; não é uma medição de GPU.

## Comparação de adaptadores e alvo de otimização

No Edge em primeiro plano, usando a GPU NVIDIA Ampere, o mesmo estado Cruise em 640×512 e `PS2_CHUNK_WRITES=1` completou uma janela de 120 quadros com **57,22 FPS produzidos e 57,22 FPS exibidos**. O tempo mediano entre quadros foi 13,8 ms; o percentil 95, 49,9 ms. O trabalho GS medido foi 5,4 ms por quadro, com estimativa GPU de 5,0 ms. Portanto a meta de aproximadamente 60 FPS é atingida nessa máquina e nessa cena, embora haja engasgos e a campanha ainda não tenha sido medida. A leitura pontual do painel, 59,6 FPS, ficou próxima da média da janela.

No navegador integrado, o adaptador escolhido é a Intel Gen12-LP. A mesma cena ficou próxima de **6,5 FPS produzidos e 3,3 FPS exibidos**, com cerca de 109–131 ms de trabalho GS por quadro. O servidor/VPS distribui os arquivos; a GPU do navegador do jogador executa a renderização. A diferença entre as duas medições não demonstra uma correção universal do renderer. Para máquinas com GPU integrada, o próximo alvo é substituir o loop raster compute por attachments nativos de cor/profundidade e texturas decodificadas, preservando o fallback GS onde os estados não forem equivalentes.

No quadro 6351, o perfil isolado contém 1.588 comandos GPU; o grafo conservador de recursos estima 1.387 níveis dependentes. Os dois lotes raster mais caros da amostra somaram 19,6 ms no perfil instrumentado. O maior deles tem **624 primitivas, 896 tiles e 13.623 entradas** nas listas de tiles. Seu rastreio nativo registra 608 triângulos PSMT4 com teste Z e blend, mais 16 sprites com outro estado. Migrar apenas os 43 lotes simples PSMT8 cobre 3,7% das primitivas dos três quadros analisados; uma solução para a Intel precisa também tratar esses lotes grandes e mistos, preservando ordem de blend e dependências da VRAM. Os tempos do perfil isolado não são tempos do renderer normal.

Ao associar 230 lotes do perfil raster do quadro 6351 aos estados do rastreio nativo do quadro desenhado 6350, os lotes **sem blend** representam só **20,69 de 150,61 ms (13,7%)** do tempo raster associado. Lotes com flags uniformes representam 61,1% e lotes com um único formato de textura, 65,2%. Portanto, uma primeira passagem limitada a desenho opaco não pode eliminar a maior parte do custo; o protótipo de attachments deve incluir os modos de blend comuns e dividir lotes mistos na ordem original. A associação de quadros decorre da diferença de um quadro entre a contagem de apresentação do perfil e a contagem nativa do GS; os 150,61 ms não são a duração real de um quadro normal.
