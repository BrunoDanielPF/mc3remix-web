# Plano de estabilidade e desempenho em GPUs mais fracas

Atualizado em 03/11/2026. Este plano usa o relato de três máquinas fornecido pelo usuário e os perfis já registrados em [PERFORMANCE.md](PERFORMANCE.md), [PERFORMANCE_CHECKPOINT.md](PERFORMANCE_CHECKPOINT.md) e [GS_RENDER_FAST_PATH.md](GS_RENDER_FAST_PATH.md). Os novos números são observações do painel, não execuções controladas do `PS2_BENCHMARK`.

## Leitura dos novos resultados

| Arquitetura | Resolução interna | FPS na tela relatado | Indicador GPU | Indicador GS |
| --- | ---: | ---: | ---: | ---: |
| NVIDIA Pascal, GTX 1050 Ti | 640 × 512 | 30 | 28% | 55% |
| AMD GCN-5, Vega | 640 × 512 | 24 | 64% | 44% |
| NVIDIA Ampere | 1280 × 1024 | 60 | 18% | 73% |

O painel calcula `GPU %` a partir do tempo acumulado de consultas de timestamp dividido pelo tempo de parede. É uma **estimativa de tempo GPU**, não ocupação medida pelo driver; a extrapolação pode inclusive ultrapassar o tempo de parede. `GS %` é **tempo ocioso da thread GS**. Os percentuais não permitem concluir, sozinhos, que uma placa esteja saturada ou que o GS seja o único gargalo. O WebGPU define consultas de timestamp como recurso opcional; disponibilidade e precisão variam por adaptador e navegador ([especificação WebGPU](https://gpuweb.github.io/gpuweb/#timestamp-query)).

A Ampere foi medida com **quatro vezes mais pixels** que as outras duas. Seu resultado confirma folga nessa máquina e nessa cena, mas não quantifica a vantagem de arquitetura em condições iguais. A GTX 1050 Ti toca o piso de 30 FPS sem margem conhecida para quedas. Para a Vega passar de 24 para 30 FPS, a duração média do quadro precisa cair de aproximadamente 41,7 para 33,3 ms: **20% menos tempo por quadro**, equivalente a **25% mais FPS**. Ainda faltam percentis, cena exata, versão do build e duração dos testes.

## O que já podemos estimar sem três computadores

Uma máquina basta para ordenar hipóteses, comparar versões no mesmo estado e rejeitar mudanças que pioram a imagem ou a cadência. A validação em outros adaptadores continua necessária antes de afirmar compatibilidade ampla, porque o custo relativo de compute, memória e apresentação varia entre drivers. Se a única máquina for a Ampere já perto de 60 FPS, medir tempos por etapa e uma cena mais pesada: o limite de apresentação pode esconder melhora no contador de FPS.

| Candidato | Evidência local | Potencial e incerteza |
| --- | --- | --- |
| Substituir o raster por pixel/tile por draws em attachments de cor e Z, mantendo grupos residentes | O shader `raster` percorre as primitivas de cada tile para cada pixel; raster responde por cerca de 70–74% dos tempos **isolados** de operações na Intel. Os cinco lotes mais caros concentram 41% do tempo raster isolado no Cruise. | **Maior potencial estrutural.** A fração medida com passagens separadas não é a fração do tempo total nem uma previsão de FPS na Vega. Requer equivalência de blend, Z, CLUT e VRAM. |
| Evitar materialização/decodificação repetida entre draws de um grupo | O quadro Cruise tem centenas de uploads, cargas CLUT, decodes e snapshots intercalados; o grafo marca 1.387 níveis dependentes. | **Potencial alto junto com o novo raster.** Cache isolado por submissão reduziu somente 3,2% dos decodes; reutilização precisa respeitar versões de páginas e feedback. |
| Reduzir comandos de upload/CLUT no renderer atual | A fusão experimental retirou 28,7% dos dispatches totais. | **Impacto isolado baixo no FPS medido.** O navegador integrado permaneceu perto de 3,3 FPS exibidos; não tratá-la como solução principal. |
| Pré-carregar disco, reduzir resolução acima de 1× ou limitar a 30 FPS | Pré-carga zerou leituras HTTP na janela sem ganho relevante de FPS; 640 × 512 já é 1×; limitar FPS só controla cadência. | **Ajuda carga, qualidade ou estabilidade em situações específicas; não demonstra ganho de FPS para a Vega a 1×.** |

Para calibrar a ordem de grandeza sem prometer resultado: se **todo o tempo por quadro** da Vega cair 10%, 20% ou 30%, os 24 FPS virariam aproximadamente **26,7, 30 ou 34,3 FPS**. Isso é apenas uma conversão matemática; nenhum protótipo atual demonstrou essas reduções nessa GPU. O primeiro objetivo experimental é encontrar cerca de **8,3 ms por quadro** e conferir que o `onScreenFps` também sobe. O perfil isolado indica onde investigar, mas só um A/B no navegador separa o ganho real de mudanças de contador ou de agendamento.

## Primeiro experimento implementado

- A tela inicial oferece `Desempenho experimental` em 640 × 512 com filtragem de textura por vizinho mais próximo, além de `Padrão` em 640 × 512 e níveis de 2×/3×. O perfil simples troca leituras bilineares por uma única amostra; a aparência muda e o ganho de FPS ainda precisa de A/B. A escolha fica salva no navegador e é aplicada antes de iniciar o runtime. No rastreio do quadro 6350, 164 de 230 lotes tinham ao menos uma primitiva com filtragem bilinear no modo original; no modo simples, nenhum manteve essa flag. Isso confirma a aplicação da opção, não um ganho de FPS.
- `PS2_ATTACHMENT_PASS=1` ativa uma prova controlada de color/depth attachments para o primeiro lote elegível de 77 primitivas PSMT8 do quadro 6350. A conversão VRAM → attachments → VRAM cobre apenas o retângulo de 352 × 86 pixels desse lote, 13,2% da área de 512 × 448. O hash completo de VRAM após o lote foi idêntico ao caminho compute: `598d61c259a58b1a` no teste nativo. A opção permanece desligada por padrão, pois converter a cada lote não é uma rota de desempenho e ainda faltam blend, grupos residentes e validação no navegador.
- `engine/scripts/gpu_hotspot_join.py` associa o perfil por comando aos metadados dos lotes e recusa correspondências com contagem de primitivas/tiles divergente. No perfil nativo Cruise, o lote mais caro tem 624 primitivas, 13.623 entradas de tiles e 11,47 ms de tempo raster **isolado** no llvmpipe. Esses milissegundos de GPU por software não estimam o tempo na Vega, GTX ou Ampere; o valor do relatório é localizar o tipo de lote que deve orientar o próximo desenho do renderer.

Exemplo de associação, com os arquivos produzidos por `PS2_GPU_PROFILE` e `PS2_GPU_FASTPATH_TRACE` no mesmo estado:

```sh
python3 engine/scripts/gpu_hotspot_join.py raster-profile.txt fastpath-batches-6351.csv --profile-frame 6351 --trace-frame 6350 --spillover-frame 6351 --rows 10
```

O próximo protótipo precisa conservar os attachments entre lotes compatíveis e cobrir os modos comuns de blend na ordem GS. Só então vale comparar A/B de `onScreenFps`, `screenMsP95` e tempo de GPU na mesma máquina e no mesmo estado.

Não antecipar o teste do retângulo da primitiva para antes da carga de estado GS como otimização geral: o histórico do renderer registra regressão por divergência entre threads nessa ordem. Essa tentativa foi revertida antes de adicionar o seletor gráfico.

## Primeiro ciclo: tornar os resultados comparáveis

1. Registrar hash ou commit do build, navegador/versão, sistema e driver, adaptador efetivamente selecionado, resolução, energia ligada à tomada, visibilidade da aba, modo solo/LAN e cena. Não executar outra instância do jogo durante o teste solo.
2. Repetir o mesmo estado de San Diego Cruise em 640 × 512 nas três máquinas. Usar a janela reproduzível do [checkpoint](PERFORMANCE_CHECKPOINT.md#cenário-de-referência), descartando aquecimento; repetir três vezes por máquina e guardar as linhas `PS2_BENCHMARK`. Um estado salvo fora do Git é necessário para a comparação exata.
3. Medir também um menu, uma corrida real e a transição de entrada/saída de corrida. Para estabilidade, registrar se a imagem e os contadores de progresso do jogo avançam; o contador de quadros sozinho não prova que o jogo saiu de um carregamento. Usar o diagnóstico de travamentos já descrito em [LOADING_CRASH_INVESTIGATION.md](LOADING_CRASH_INVESTIGATION.md) quando houver congelamento.
4. Comparar `onScreenFps`, `screenMsP50/P95`, `fps`, `frameMsP50/P95`, `gsWorkMsPerFrame`, `presentWaitMsPerFrame`, `rafRateHz` e leituras de disco. Avaliar áudio, resposta dos controles e saves durante uma sessão longa; FPS médio isolado não basta.
5. Repetir em 1280 × 1024 apenas nas máquinas que sustentarem 640 × 512. A resolução 640 × 512 já é o menor nível oferecido pelo renderer atual; não assumir que um seletor abaixo de 1× seja uma alteração simples ou fiel ao GS.

## Ordem de trabalho

### 1. Estabilidade funcional e observabilidade

- Reproduzir e corrigir os travamentos/esperas de IPU e a falha de callback na transição Arcade com os estados e sondas existentes. Exigir que abertura, menus, Cruise, corrida, retorno ao menu e save avancem durante sessões longas. O diagnóstico atual ainda não demonstra uma correção segura para esses casos.
- Tratar perda de dispositivo WebGPU, falta de memória e falhas de compilação com diagnóstico claro e recuperação testada. Registrar o adaptador realmente usado e distinguir a cópia para canvas e o renderer por software, pois esses caminhos invalidam comparações diretas.
- Padronizar o benchmark entre máquinas antes de alterar o caminho de renderização. Exigir comparação A/B no mesmo estado e build, com captura de imagem e profundidade quando o GS mudar.

### 2. Ganho de FPS que alcance a Vega e GPUs integradas

- Priorizar os lotes raster grandes e mistos do Cruise. O perfil existente tem cerca de 1.588 comandos GPU por quadro e um lote de 624 primitivas/13.623 entradas de tiles; só 13,7% do tempo raster associado está em lotes sem blend. O protótipo de draw por primitiva cobre apenas 3,7% das primitivas dos três quadros analisados e não melhorou o FPS exibido.
- Criar texturas decodificadas como `GPUTexture` e color/depth attachments residentes. Agrupar draws que compartilham framebuffer, mantendo a ordem GS; materializar páginas swizzled de VRAM somente quando uma operação posterior realmente precisar lê-las. Cobrir blend comum e dividir lotes mistos mantendo a ordem. Conservar fallback compute para modos ainda não equivalentes.
- Medir separadamente encode/submit WebGPU, trabalho GS, espera de apresentação e callbacks do navegador. Fusão de upload/CLUT, cache e redução de dispatches só entram no padrão se reduzirem **FPS exibido e cauda de latência** em A/B; os experimentos anteriores reduziram comandos sem ganho suficiente.

### 3. Modo para máquinas mais fracas

- Manter 640 × 512 como ponto de partida e escolher uma GPU física quando a máquina tiver opção dedicada. Verificar o nome do adaptador e se houve fallback/cópia de canvas antes de classificar a máquina como lenta.
- Depois de validar o novo renderer, oferecer um perfil `Compatibilidade/30 FPS` que selecione apenas caminhos comprovadamente mais rápidos nessa classe de GPU. Um limite de 30 FPS pode melhorar cadência e temperatura, mas **não transforma 24 FPS em 30**; testar sua relação com áudio, entrada e LAN antes de oferecê-lo.
- Considerar renderização remota com GPU/streaming somente como produto separado, após medir custo, latência e qualidade de rede. O VPS atual entrega arquivos e pacotes LAN; sua GPU não renderiza os quadros do navegador do jogador.

## Portões de aceitação

- **Estabilidade:** completar uma sessão prolongada com boot, navegação, Cruise, corrida, saída, save e recarga, sem congelamento de progresso, perda de áudio ou dispositivo não tratado.
- **Desempenho mínimo:** em 640 × 512, pelo menos 30 FPS **exibidos** na Vega e na GTX 1050 Ti nas cenas escolhidas, com `screenMsP95` e pausas longas registrados. A meta posterior é 60 FPS nas GPUs com margem. Não aprovar uma mudança só por FPS produzido ou por uma leitura instantânea do painel.
- **Correção gráfica:** comparar cor e Z com o renderer de referência nos quadros Cruise já usados, além de menu e corrida real; validar blend, textura/CLUT, feedback de framebuffer e transições. Manter cada caminho experimental desligado por padrão até passar pelos testes.
- **Regressão:** repetir solo e LAN, resolução 1× e 2× onde aplicável, em NVIDIA, AMD e GPU integrada. Publicar somente depois da validação do pacote final no navegador e de uma rota de rollback.
