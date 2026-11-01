# Ponto de retomada: desempenho em GPUs integradas

Atualizado em 28/10/2026. Meta: **60 FPS exibidos**, com **30 FPS exibidos** como mínimo jogável. Os números abaixo são de janelas curtas no mesmo estado salvo de Arcade → San Diego → Cruise; não representam campanha, corrida com adversários nem todas as GPUs integradas. Ver [medições completas](PERFORMANCE.md) e [plano do renderizador](GS_RENDER_FAST_PATH.md).

## Cenário de referência

- ISO PS2 Remix `SLUS_213.55`; estado local `cruise-6300.state`; entrada `6310:cross:220`; 640×512; quadros 6350–6470; `PS2_CHUNK_WRITES=1`.
- URL de benchmark: `http://127.0.0.1:8620/?state=cruise-6300.state&press=6310:cross:220&bench-from=6350&bench-frames=120&PS2_CHUNK_WRITES=1`. A ISO e o estado ficam fora do Git.
- Intel Gen12-LP no navegador integrado: aproximadamente **6,5 FPS produzidos / 3,1–3,4 FPS exibidos**. A aba/canvas precisa estar visível. O `requestAnimationFrame` foi medido perto de 3,28 Hz sob essa carga.
- Edge em primeiro plano com NVIDIA Ampere: **57,22 FPS produzidos e exibidos** na mesma cena, mediana 13,8 ms e P95 49,9 ms. Isso confirma viabilidade nessa GPU/cena; a cauda e a campanha ainda exigem medição.
- O VPS distribui arquivos e poderá encaminhar pacotes de rede. **A GPU do jogador renderiza os quadros.** Portanto, um VPS pequeno não elimina o gargalo da Intel.

## Estratégias já testadas

| Tentativa | Evidência | Decisão |
| --- | --- | --- |
| Pré-carregar 55 blocos da ISO (13,75 MiB) | 41/41 buscas atendidas sem HTTP; 3,30 FPS exibidos contra 3,27 sem pré-carga | Útil para latência de streaming, sem ganho de FPS nesta cena |
| Dividir escritas GPU (`PS2_CHUNK_WRITES=1`) | 3,36/3,36 contra 3,21/3,11 FPS exibidos em duas janelas; quatro capturas iguais | Ganho pequeno, experimental; não resolve a meta |
| Fundir upload e CLUT (`PS2_FUSE_PRE=1`) | −28,7% de dispatches; 3,34/3,34 contra 3,11/3,13 exibidos; quatro capturas iguais | Ganho pequeno; não se soma claramente aos blocos |
| Reordenar preparações independentes | Menos uploads/CLUT, capturas iguais, sem ganho relevante de FPS | Desligado por padrão |
| Esperar apresentação a cada dois quadros | 2,93 contra 3,16 FPS exibidos em comparação curta | Restaurada espera a cada quadro |
| Desligar decode / consultas de timestamp | Decode desligado: 2,77 contra 3,16 FPS exibidos; timestamps desligados: 3,03 contra 3,42 em Edge com Intel | Não há ganho reproduzido |
| Dobrar a resolução para 1280×1024 | 1,87 contra 3,16 FPS exibidos | Aumenta custo; manter 640×512 no benchmark |
| Omitir cópia para o canvas (`nosurface=1`) | 6,95 contra 6,59 FPS **produzidos**, mas sem imagem visível | A cópia final não explica o gargalo |
| Cache de decode por submissão | −3,2% de decode; 3,40 contra 3,27 FPS exibidos em amostra única | Não comprovado como otimização geral |
| Patch upstream de cache e cache persistente de textura | Patch foi ~4–6% pior no menu; cache persistente lotou e elevou decodes | Não incorporar sem novo desenho |
| Reduzir grupo raster de 256 para 64 threads | Capturas iguais; 2,50 contra 3,37 FPS exibidos | Variante retirada |
| Mover teste de caixa no shader / pré-calcular formatos na CPU | Caixa: 2,88 contra 3,29 FPS; formatos: 3,31 contra 3,33; capturas iguais | Variantes retiradas |
| Especializar raster de sprites | 3,44–3,46 contra 3,32–3,39 FPS exibidos | Ganho insuficiente, desligado |
| Dividir lotes mistos (`PS2_FASTPATH_SPLIT=1`) | Cobertura elegível aumenta, mas dispatches também; 2,99 contra 3,32 FPS exibidos | Ferramenta de preparação, não ligar como otimização |
| Render pass por lote (`PS2_RASTER_PASS=all`) | Seis capturas nativas idênticas; 2,63–2,66 contra 3,25 FPS exibidos | Prova de correção; custo maior porque ainda escreve VRAM pixel a pixel |
| Draw por primitiva (`PS2_PRIMITIVE_PASS=all`) | 53 capturas iguais; 43 lotes/835 primitivas, 3,7% do trecho; 3,14–3,18 contra 3,40 FPS exibidos | Prova de correção; cobertura e modelo de framebuffer insuficientes |
| Remover todo o raster (diagnóstico inválido visualmente) | Só 22–23 FPS produzidos e ~11,4 exibidos | Raster é dominante, mas removê-lo isoladamente não basta |

O quadro 6351 tem **1.588 comandos GPU** e **1.387 níveis** no grafo conservador de dependências. O perfil isolado atribui apenas **13,7% do tempo raster associado** a lotes sem blend. O maior lote observado mistura 608 triângulos PSMT4 e 16 sprites, com 624 primitivas e 13.623 entradas de tiles. Migrar somente lotes opacos simples não alcança a cobertura necessária. Os tempos do perfil isolado não são tempos de um quadro normal.

## Próximo experimento que ainda falta validar

1. Criar texturas decodificadas como `GPUTexture` e color/depth attachments nativos para o framebuffer/Z, começando por um lote controlado. O draw atual ainda chama `shade_at`/`plot` e escreve a VRAM em storage buffer; trocar apenas a chamada de compute por render pass já foi testado e regrediu.
2. Manter attachments por **grupos** de lotes; materializar as páginas swizzled da VRAM apenas quando um comando posterior realmente ler os bytes. Validar invalidação de textura/CLUT e feedback. Converter por lote provavelmente preservaria o custo atual.
3. Implementar modos comuns de blend do PS2 e dividir lotes caros/mistos sem alterar a ordem GS. Manter fallback compute para estados não equivalentes.
4. Comparar hash/captura completa de cor e Z nos quadros 6320/6340/6360/6380, menu e uma corrida real. Repetir benchmark A/B no mesmo navegador, GPU, resolução, cena e janela; medir FPS **exibidos**, P50/P95, áudio, entrada e salvamento.
5. Separar possível limitação do navegador integrado: medir também Edge/Chrome com a **Intel realmente selecionada**, janela visível e sem outra aba do jogo competindo pela GPU. Avaliar agendamento de apresentação após reduzir os comandos GS. O dado atual não prova que toda GPU integrada ficará em 3 FPS.
6. Antes de publicar multiplayer, comparar solo e duas instâncias. A rede não deve degradar o modo solo; medir latência e largura de banda no VPS quando houver ponte LAN do PS2.

**Critério para ativar por padrão:** captura equivalente nas cenas de teste e melhora repetível de FPS exibido/P95 sem regressão de entrada, áudio ou save. Até lá, os caminhos experimentais permanecem opt-in. Não há evidência atual de 30 FPS na Intel.
