# Próximo caminho gráfico para Midnight Club 3 Remix

## Por que mudar o renderizador

No Cruise 640×512, o navegador integrado exibe aproximadamente 3,1–3,4 FPS. O renderer atual representa a VRAM swizzled do GS em um buffer e usa dispatches WebGPU compute para uploads, CLUT, decodificação, snapshots e rasterização. O quadro 6351 teve 1.588 comandos. Seu grafo conservador de acessos possui 1.387 níveis de dependência; mesmo considerando só a VRAM, são 1.087 níveis. Reordenar os comandos existentes oferece pouco paralelismo neste trecho. A divisão de gravações em blocos e a fusão de pré-operações deram ganhos pequenos e não se somaram.

O `nfsu2-web` parte de um jogo de PC que já emite desenhos Direct3D 9; seu host traduz essas chamadas para WebGPU. Aqui a CPU do PS2 foi recompilada, mas as chamadas são comandos do GS, com formatos e feedback de VRAM próprios. Para buscar 30–60 FPS, precisamos traduzir os desenhos GS comuns para o pipeline gráfico WebGPU e reservar o caminho compute atual para os casos que requerem a semântica exata da VRAM.

## Cobertura medida

`PS2_GPU_FASTPATH_DIAG=1` em 81 quadros nativos a partir do estado 6300, com X pressionado a partir de 6310, registrou **521.500 primitivas**. São contagens de primitivas, não tempo de GPU nem prova de compatibilidade com o pipeline gráfico.

| Condição cumulativa | Primitivas | Parcela do total |
| --- | ---: | ---: |
Sem snapshot de framebuffer | 503.797 | 96,6% |
Acima e formato de framebuffer 32/24 bits | 502.074 | 96,3% |
Acima, sem blend nem teste de alfa de destino | 88.816 | 17,0% |
Acima, teste de alfa com falha KEEP ou desligado, sem máscara de framebuffer, triângulo/sprite | 66.654 | 12,8% |
Seleção mais ampla: sem snapshot/DATE/máscara, formato 32/24 bits, falha de alfa KEEP ou teste desligado, triângulo/sprite | 346.710 | 66,5% |

Todas as 346.710 primitivas da seleção ampla são texturizadas. Entre elas, 136.678 usam a equação de mistura `0x44`, e 62.145 usam `0x0a` ou `0x2a` (mistura identidade). Na cena inteira, os modos de falha do teste de alfa têm distribuição KEEP 394.986, FB_ONLY 50.308, ZB_ONLY 0 e RGB_ONLY 43.950. Um caminho somente opaco perderia quase toda a cena. Os modos `0x44`, `0x0a` e `0x2a` são prioridades para um protótipo, sem presumir equivalência bit a bit com a mistura fixa do WebGPU.

A seleção por primitiva superestima o que pode ser enviado sem dividir lotes: só **3.768 de 14.538 lotes inteiros**, com **97.004 primitivas**, atendem à seleção ampla. Nenhum desses lotes dispensa o teste de profundidade. No grafo do quadro 6351, **207 dos 230 intervalos entre rasterizações** exigem comandos intermediários; apenas 23 são candidatos otimistas a movimentação. Portanto o protótipo precisa aceitar profundidade e preservar a ordem dos uploads, CLUT, decodificações e feedback, mesmo que inicialmente use uma passagem gráfica por lote.

`PS2_GPU_FASTPATH_TRACE=<início>,<fim>,<arquivo.csv>` exporta um registro por lote, com formatos, modos, cobertura e tiles, sem alterar o renderizador. `scripts/gpu_fastpath_trace.py` agrupa os registros para escolher um lote inteiro e repetível. No trecho nativo 6350–6352, os quadros 6350 e 6352 tiveram **458 lotes, 22.440 primitivas e 25.479 tiles**; o quadro 6351 registrou apenas um lote e não entrou na comparação de cobertura. Entre os dois quadros de desenho, **130 lotes completos, 3.985 primitivas**, passaram pela seleção ampla, todos texturizados. Os maiores grupos uniformes foram:

| Formato de textura e mistura | Lotes | Primitivas | Tiles |
| --- | ---: | ---: | ---: |
| PSMT8, sem mistura | 43 | 835 | 870 |
| PSMT4, sem mistura | 41 | 696 | 159 |
| PSMT4, mistura `0x01` | 6 | 195 | 41 |

Esses grupos usam framebuffer PSMCT32, profundidade PSMZ16S, comparação Z `2` e escrita de Z. Há lotes maiores com várias texturas ou modos de mistura: o primeiro protótipo isolado terá cobertura pequena até suportar divisão interna e coerência de VRAM. As contagens são cobertura de comandos, não previsão de FPS.

`PS2_FASTPATH_SPLIT=1` divide o lote quando a sequência muda entre primitivas elegíveis e inelegíveis; o renderer compute continua desenhando todas elas. Em 81 quadros nativos, quatro capturas ficaram idênticas à base. A separação elevou os lotes completos elegíveis de **3.768 / 97.004 primitivas** para **9.950 / 346.710 primitivas**. Ela criou 10.793 fronteiras e elevou os dispatches raster de 14.538 para 24.222; as cargas de CLUT passaram de 14.882 para 23.610, enquanto as decodificações caíram de 11.905 para 5.467. No navegador integrado, o FPS exibido foi **2,99 com divisão e 3,32 na base**, em uma janela isolada por versão. O recurso fica desligado por padrão. Ele mostra que a cobertura por primitiva pode virar cobertura por lote sem alterar a imagem, mas o caminho gráfico terá de substituir trabalho suficiente para compensar os comandos adicionais.

Nos dois quadros de desenho 6350 e 6352, o rastreio com divisão registrou **414 lotes completos elegíveis, com 15.221 de 22.440 primitivas**. O maior subconjunto ainda mistura formatos de textura e de blend dentro do mesmo lote; separar modos e reutilizar texturas renderizadas serão necessários para atingir cobertura alta. O primeiro lote uniforme PSMT8 sem mistura continua sendo um alvo de validação, não o ponto de chegada.

## Testes em outras cenas

No menu de criação de perfil (`perf-4100.state`, até o quadro 4160), três capturas PPM com divisão ficaram idênticas às da base. O rastreio completo contou **274.377 primitivas** em ambas as variantes: lotes inteiros elegíveis passaram de **56.465** para **167.653 primitivas**. Os dispatches raster subiram de **4.588** para **11.997** e os de CLUT de **8.901** para **16.279**. Em janelas isoladas de 120 quadros no navegador integrado, com Intel Gen12-LP, 640×512 e `PS2_CHUNK_WRITES=1`, a base produziu **9,83 FPS e exibiu 4,34**; com divisão, produziu **7,67 e exibiu 4,35**. O FPS visível não melhorou nessa amostra, apesar da maior cobertura.

Na transição carregada de `route-5700.state` com X no quadro 5730, as capturas 5720/5740/5760/5780 também ficaram idênticas. A divisão criou 1.479 fronteiras e elevou rasterizações de 1.120 para 2.412. A cobertura visual foi verificada nesses quadros; ainda falta medir uma corrida completa.

O script de rastreio agora destaca lotes completos com PSMT8 ou PSMT4, PSMCT32, PSMZ16S, teste Z `2`, escrita Z e mistura desligada ou identidade (`0x0a`/`0x2a`). Esse subconjunto soma **1.659 de 22.440 primitivas** nos dois quadros Cruise sem divisão e **2.634** com divisão; no menu, **33.759 de 274.377** sem divisão e **61.318** com divisão. São limites inferiores para o primeiro desenho gráfico simples. A cobertura necessária para 30–60 FPS exige tratar os lotes com mistura comum e múltiplas texturas; estas contagens não estimam o ganho de tempo.

## Protótipo de implementação

1. **Texturas e coerência.** A cache de decodificação limitada a uma submissão já foi implementada como opção `PS2_DECODE_CACHE=1`: as capturas comparadas ficaram idênticas, mas a queda líquida foi de 3,2% dos dispatches de decode. Uma reserva persistente adicional de 16 MiB lotou quase a cada submissão e foi rejeitada. Para o caminho gráfico, decodificar texturas GS para `GPUTexture` com chave de formato, CLUT e versões das páginas de VRAM; invalidar ao tocar as páginas usadas e medir o custo real do espelho.
2. **Desenho direto mínimo.** Criar color/depth attachments para framebuffer 32/24 bits. Traduzir triângulos e sprites sem snapshot, DATE ou máscara de framebuffer, começando pelos modos sem mistura e de mistura identidade, com teste de alfa `AFAIL=KEEP`. Implementar o teste no fragment shader e comparar imagens completas, inclusive profundidade, recorte e arredondamento.
3. **Mistura comum.** Adicionar `0x44` e validar a escala do alfa PS2, cores acima de 128, saturação e arredondamento contra o renderer compute. Se WebGPU fixed-function divergir, usar outro shader/pass ou deixar o lote no fallback.
4. **Coerência com a VRAM.** Agrupar desenhos diretos enquanto não houver leitura de textura, feedback ou transferência que dependa do framebuffer. Na fronteira, converter o attachment para a disposição swizzled da VRAM **uma vez por grupo**; ao voltar do caminho compute, materializar a região necessária no attachment. Resolver por desenho anularia o ganho.
5. **Validação.** Usar os quadros 6320/6340/6360/6380 e pelo menos uma corrida. Exigir capturas equivalentes, entrada/áudio íntegros e melhora repetível em FPS exibido e percentis no navegador. Manter opção para desligar o caminho direto durante o desenvolvimento.

O próximo marco de implementação é um lote inteiro PSMT8 sem mistura, com Z PSMZ16S e teste `2`, isolado atrás de uma opção, com captura bit a bit contra o renderer compute. Depois será preciso dividir lotes mistos sem alterar a ordem GS. A porcentagem de primitivas acima é um teto de cobertura, não uma previsão de FPS: uploads, CLUT, sincronizações e custo de apresentação continuam presentes.

O perfil por timestamp executado no WebGPU da Intel Gen12-LP reforçou a prioridade da rasterização. Ao separar cada comando em uma passagem compute, o quadro 6351 de Cruise mediu 227 rasterizações em 179,5 ms, contra 23,7 ms de decode, 18,9 ms de CLUT e 17,6 ms de upload. No menu de perfil, 148 rasterizações somaram 154,5 ms. Os cinco lotes raster mais caros cobriram 847–1.024 tiles cada e responderam por 41% do tempo raster medido no Cruise e 51% no menu. Esse perfil altera a estrutura das passagens e bloqueia para ler os timestamps; seus valores absolutos não são tempos do renderer normal. O primeiro lote PSMT8 continua útil para provar correção, mas o ganho de FPS dependerá de alcançar os lotes grandes. Os dados e os testes de apresentação estão em [desempenho](PERFORMANCE.md).

Uma tentativa de reduzir o grupo raster de 256 para 64 threads por tile, calculando quatro pixels por thread, preservou quatro capturas nativas byte a byte, mas caiu de 3,37 para 2,50 FPS exibidos no mesmo build web. O código experimental foi retirado. Entre os lotes caros há um com 624 primitivas e mais de 13 mil entradas de tiles; 608 primitivas dele passam pela seleção ampla, mas os modos são mistos. Assim, a primeira passagem gráfica pode provar correção em um lote uniforme menor, porém o marco de desempenho precisa dividir e migrar lotes grandes como esse, preservando a ordem GS e a coerência com a VRAM.

## O que reaproveitar do nfsu2-web

O [contrato de renderização do nfsu2-web](https://github.com/th0mk/nfsu2-web/blob/main/docs/web/DESIGN.md#3-the-renderer-contract-c-recorder--typescript-webgpu-encoder) grava comandos de desenho e recursos em memória compartilhada e os consome no lado WebGPU com uploads agrupados, caches de pipeline e passagens gráficas. Adaptaremos o **contrato**, não os comandos Direct3D 9: o produtor aqui recebe pacotes GS e precisa preservar as dependências da VRAM do PS2.

| Conceito do nfsu2-web | Adaptação para MC3 Remix |
| --- | --- |
| Frame packets e fila ordenada de recursos | Registrar lotes GS, uploads, CLUT, transferências e fronteiras de feedback em uma sequência estável |
| Texturas e render targets da GPU | Espelhar regiões da VRAM em `GPUTexture` color/depth, com versões de páginas e invalidação ao escrever |
| Pipeline e bind-group caches | Chave por formato, teste Z, alpha, blend e textura/CLUT; reutilizar somente estados equivalentes |
| Draw calls em render pass | Migrar primeiro lotes completos simples; depois dividir os grandes e mistos sem mudar a ordem |
| Readback pontual | Materializar o framebuffer na VRAM apenas quando o próximo comando GS realmente o ler |

O experimento `PS2_SPRITE_FAST=1` especializou o shader compute de lotes grandes compostos só de sprites. Ele manteve quatro capturas byte a byte iguais, mas exibiu **3,44–3,46 FPS** contra **3,32–3,39** da base na janela Cruise, uma diferença pequena. Por isso fica experimental e desligado. O próximo protótipo precisa substituir o raster compute por um render pass WebGPU e medir redução de trabalho nos lotes caros; especializar mais um ramo do mesmo laço de pixels não oferece escala suficiente. Resultados completos: [desempenho](PERFORMANCE.md#shader-especializado-para-sprites-e-comparação-com-nfsu2-web).

O diagnóstico `PS2_GPU_SKIP_RASTER=1` retirou **todo** o trabalho raster na janela Cruise e produziu apenas **22,23–22,70 FPS**, com **11,35–11,49 FPS amostrados na tela**; a imagem fica inválida. A base no mesmo build produziu 6,57 e exibiu 3,34 FPS. Portanto, o primeiro render pass continua sendo uma prova de correção necessária, mas a meta de desempenho exige também agrupar operações de preparação, evitar conversões de VRAM por lote e reduzir perdas na apresentação. [Medições e limites](PERFORMANCE.md#limite-experimental-sem-rasterização).

## Resultado do primeiro render pass

`PS2_RASTER_PASS=<quadro>,<comando>` reproduz um lote GS por render pass; `PS2_RASTER_PASS=all` aplica isso a todos os lotes. O vertex shader cria um quad de 16×16 por tile e o fragment shader reutiliza o mesmo cálculo de textura, teste Z, blend e escrita de VRAM do compute. No runner nativo, seis capturas espaçadas em dez quadros até o 6360 ficaram **idênticas byte a byte** com `all`. Isso prova que a troca de mecanismo preservou a saída nesses quadros. A opção permanece desligada por padrão.

No navegador, a base exibiu **3,25 FPS**; duas execuções de `all` exibiram **2,66 e 2,63 FPS**. O trabalho GS subiu de 126,0 para cerca de 169 ms por quadro. O primeiro passo conseguiu correção, mas manteve o laço de pixels e acrescentou um render pass a cada lote; por isso não é o caminho de desempenho. [Resultados detalhados](PERFORMANCE.md#prova-de-render-pass-para-os-lotes-gs).

O próximo protótipo deve começar pelo lote inteiro PSMT8 sem mistura do quadro 6350: gerar **triângulos e sprites como draw calls**, enviar a textura decodificada a `GPUTexture`, usar attachments color/depth para o framebuffer e converter para a VRAM somente quando um comando posterior depender desses bytes. Primeiro comparar imagem completa e profundidade com o fallback compute em um lote; depois agrupar lotes entre as fronteiras de feedback. A renderização direta precisa cobrir os lotes caros e mistos para produzir ganho relevante, e a meta de 30 FPS também exige reduzir o custo residual de CLUT, uploads e apresentação.

## Desenho por primitiva e verificação da VRAM

`PS2_PRIMITIVE_PASS=1` substitui um lote uniforme do quadro 6350 por um draw instanciado: cada primitiva gera seu próprio quad de cobertura e o fragment shader aplica as regras exatas de aresta do GS. `PS2_PRIMITIVE_PASS=all` estende a seleção aos lotes uniformes PSMT8, sem mistura, com framebuffer PSMCT32 e Z PSMZ16S; o caminho normal continua sendo o padrão. Os modos diagnósticos `compute`, `skip`, `all-compute` e `all-skip` permitem comparar a VRAM imediatamente depois do primeiro lote e a imagem final.

No runner nativo, o lote de 77 primitivas produziu hash FNV-1a de 4 MiB de VRAM `598d61c259a58b1a` tanto em compute como no draw; omiti-lo produziu `14ef0ac5fd6cdfeb`. O primeiro lote da seleção ampla teve hash `32ccf3e3a1e1534f` em ambos os caminhos. Em três quadros Cruise, **43 lotes / 835 primitivas / 870 tiles** passaram pela seleção ampla, de 22.440 primitivas e 25.479 tiles no rastreio: **3,7% das primitivas**. As 53 capturas consecutivas até o quadro 6353 foram idênticas byte a byte; ao omitir os 43 lotes, o quadro 6353 mudou. Esses testes validam a saída da seleção nessa cena, mas ainda não demonstram correção em outras cenas ou em sobreposições mais complexas. Os 43 lotes não formam pares consecutivos de comandos; o menor intervalo entre eles é quatro comandos. Portanto, agrupar passagens gráficas exige também tratar as operações intermediárias e modos GS adicionais.

No navegador integrado, Intel Gen12-LP, 640×512, Cruise, 120 quadros e `PS2_CHUNK_WRITES=1`, a seleção ampla produziu **6,47 FPS / 3,18 FPS exibidos**; a base produziu **6,58 / 3,40**. Com o registro de cada lote suprimido no navegador, a seleção ampla produziu **6,61 / 3,14**. Essas diferenças pequenas não constituem ganho. O shader ainda usa `shade_at` e `plot` para ler/escrever a VRAM como buffer de armazenamento. A próxima alteração deve manter color/depth attachments residentes por uma sequência de lotes e materializar suas páginas na VRAM quando houver uma leitura dependente. Converter a cada lote tende a acrescentar barreiras e a repetir o custo atual. Para chegar a 30–60 FPS, será preciso cobrir os lotes caros e mistos; esta seleção simples é só uma prova de correção.
