# Plano técnico: MC3 Remix USA de PS2 para web

Atualizado em 27/10/2026. A cópia foi identificada como *Midnight Club 3: DUB Edition Remix*, PlayStation 2 NTSC-U, executável `SLUS_213.55`. O relatório do [README](../README.md) contém o tamanho e o SHA-256 da ISO. Isso identifica os arquivos locais; não certifica origem ou integridade frente a uma referência externa.

## Correção da base técnica

O plano inicial considerava a edição de Xbox. A ISO encontrada tem `SYSTEM.CNF` e o executável de PS2 `SLUS_213.55`; não tem o `default.xbe` requerido pelo [`mcdub3-nx`](https://github.com/laureladastra/mcdub3-nx). Portanto o seu elevador Xbox e o backend NV2A não servem para esta imagem.

A base mais próxima do objetivo de recompilação para navegador é [`ps2-web-recomp`](https://github.com/snuri00/ps2-web-recomp), revisão `333999564ad0fe84f24b504416869dc182404a61`. Ela:

- lê `SYSTEM.CNF`, extrai o executável EE R5900 e gera C++ por unidades de código;
- compila o código e o runtime de PS2 para WebAssembly com Emscripten;
- executa módulos IOP, VU1 e GS; desenha o GS com WebGPU e usa AudioWorklet;
- serve a ISO por solicitações HTTP Range em `localhost`, com headers COOP/COEP para memória compartilhada;
- mantém o cartão de memória no armazenamento do navegador.

As demonstrações declaradas pelo projeto são *Gran Turismo 4* e *God of War II*. A própria documentação avisa que novos jogos podem parar em chamadas de sistema, bibliotecas IOP ou recursos GS ainda não cobertos. O MC3 Remix precisa de prova própria, sem presumir que chegará à corrida. Fontes: [README e método de porte](https://github.com/snuri00/ps2-web-recomp#port-a-game-of-your-own), [compatibilidade Play! do mesmo serial](https://github.com/jpd002/Play-Compatibility/issues/965). O relatório do Play! é de outra implementação e sua última prova registrada é de 2022; serve apenas como indício de complexidade, não como resultado deste porte.

## Prova local

O build de teste foi feito em WSL Ubuntu 24.04, em diretório separado do repositório. A ISO original permaneceu fora da árvore versionada; o script criou um link simbólico no WSL em vez de copiar os 8,54 GB.

Pacotes de build instalados no WSL: CMake, Ninja, Clang 19, libc++, SDL2. O script oficial instala Emscripten SDK 6.0.11 no checkout de trabalho e busca wgpu-native para o runner nativo.

Primeiro comando executado:

```bash
cd "$HOME/mc3remix-build"
scripts/port.sh mc3remix "/caminho/para/sua-copia/MC3-Remix.iso" --frames 1200 --rounds 3
```

Depois, a exploração foi ampliada para `--frames 4200 --rounds 4`. As duas execuções concluíram a geração e os builds nativo e web. A segunda gerou 275 unidades C++, encontrou 202 microprogramas VU1 e 144 alvos de salto. O runner nativo executou 4.200 frames sem função ou overlay ausente. Capturas mostraram a tela `PRESS START BUTTON` e o aviso de cartão de memória não formatado. Uma execução sem entradas ficou no último quadro da introdução, assim como o navegador; entradas `Start` e `X` durante os logotipos avançaram até o título.

No navegador, a página iniciou e desenhou a abertura com WebGPU próximo de 60 FPS. Reproduzi as entradas do runner nativo (`Start` e `X` durante os logotipos) e cheguei à tela de título, ao aviso de cartão, ao menu principal e a **Arcade → San Diego → Cruise**. O jogo carregou a cidade e o Eclipse, com HUD e movimento visível. O desempenho caiu para cerca de 10–17 FPS nos menus e 5–8 FPS no Cruise, no navegador integrado desta máquina. Ainda não medi uma corrida com adversários nem validei som, saves ou uma sessão longa.

O console registrou cinco mensagens `IPU bad code`. Um rastreamento nativo até 3.000 frames registrou as mesmas cinco, nos mesmos índices de comando e com os mesmos bits de entrada. Elas são uma limitação do decodificador atual, mas não explicam uma diferença entre os dois ambientes nessa abertura. O servidor devolveu HTTP 206 e bytes corretos em amostras do começo, meio, acima de 4 GiB e fim da ISO. O host web agora checa `Content-Range` e o comprimento de cada resposta; o teste não mostrou aviso de leitura.

**Estado:** o navegador alcança um modo de direção local. No Cruise, o painel de diagnóstico mostrou `gs idle` perto de 0% e `GPU` perto de 3%, sinal inicial de carga no processamento do GS antes da GPU; ainda falta perfil por função para confirmar o gargalo. Os próximos passos são medir uma corrida com adversários, corrigir a baixa taxa de quadros e os erros de IPU, validar o áudio e o cartão de memória e repetir a prova em Chrome ou Edge. As capturas nativas, o rastreamento e o console são artefatos locais, fora do Git.

Em 26/10, uma revisão upstream mais recente (`79d8eab`) também foi compilada e inicializada em checkout separado. Ela renderizou a tela de perfis, mas não mostrou melhora evidente de FPS nessa tela. A correção de cache de texturas do commit upstream `b837b2e` foi testada isoladamente no mesmo estado salvo; as duas amostras de 120 quadros ficaram mais lentas, então ela não foi mantida na fonte principal. O shell recebeu uma medição opcional por janela de quadros. O [protocolo de desempenho](PERFORMANCE.md) registra os números e explica quando considerar uma mudança maior na recompilação.

O Cruise agora tem um estado reproduzível no quadro 6300. Com aceleração pelo botão X, uma janela visível de 120 quadros após aquecimento mostrou cerca de 6,5 FPS produzidos e 3,2 FPS exibidos. O perfil nativo revelou aproximadamente 1.575 dispatches WebGPU por imagem; uma execução finita no navegador confirmou dezenas de milhares de uploads, cargas de CLUT, rasterizações e decodificações. Testes em Edge e com opções de decodificação, resolução, timestamps e espera não se aproximaram de 30 FPS exibidos. O diagnóstico de uploads mostrou que um cache conservador por página de VRAM descartaria apenas 8 de 30.052 uploads. Os detalhes, limites da medição e a sequência de botões estão no [relatório de desempenho](PERFORMANCE.md).

Uma análise posterior do quadro 6351 encontrou 1.387 níveis de dependência entre 1.588 comandos GS, mesmo antes de incluir todas as barreiras da CPU. Dividir as gravações WebGPU em blocos de 1 MiB preservou quatro capturas nativas, mas elevou o FPS exibido somente de aproximadamente 3,1–3,2 para 3,36 no trecho testado. O perfil de estados identificou 346.710 de 521.500 primitivas como seleção ampla para um renderizador por pipeline gráfico, incluindo mistura e teste de alfa comuns. O próximo trabalho de implementação é descrito em [caminho gráfico direto](GS_RENDER_FAST_PATH.md); essa cobertura é diagnóstica e ainda não mede ganho de FPS.

A análise dos lotes reduziu a cobertura imediata: só 97.004 primitivas estão em lotes inteiros elegíveis e todos usam teste de profundidade. Uma cache de textura limitada à submissão preservou quatro capturas nativas e reduziu os dispatches de decodificação em 3,2%; a primeira comparação no navegador passou de 3,18 para 3,34 FPS exibidos. Ela permanece optativa. Uma reserva persistente de 16 MiB foi testada e retirada por aumentar o trabalho de decodificação. O próximo protótipo precisa suportar Z desde o início e comparar imagens antes de qualquer ativação padrão.

O código fonte de `ps2-web-recomp` foi incluído em [`../engine/`](../engine/) como snapshot do commit acima, preservando sua licença MIT. As mudanças locais incluem validação da leitura HTTP e contadores do loop gráfico em `host/web.cpp`, exportação desses contadores em `CMakeLists.txt`, rastreio multirrecurso e protótipos opcionais de GPU em `runtime/src/gs/gpu/`, scripts de análise em `scripts/` e ajustes de título, ajuda, layout e benchmark em `web/shell.html`. A instrumentação temporária do IPU foi removida após a comparação. O build WSL permanece separado para não colocar dependências e dados do jogo na pasta OneDrive.

## Arquitetura para hospedagem futura

O VPS servirá os arquivos da aplicação; EE, VU, GS e WebGPU executam no navegador do visitante. Portanto uma GPU no VPS não aumenta o FPS do cliente. A prévia atual usa um servidor local com HTTP Range para a ISO de 8,54 GB e não é uma distribuição pública pronta.

Antes de publicar, a aplicação deve permitir que cada usuário selecione sua própria imagem local e mantê-la no armazenamento local do navegador, sem upload ao VPS. O host poderá servir o shell, JavaScript e WebAssembly com os headers de isolamento exigidos pelas threads, após verificar os direitos de distribuição dos artefatos derivados do jogo. O fluxo de ISO local, cache, atualização e retomada ainda precisa ser implementado e testado. O requisito de 30 FPS exibidos deve ser validado em navegadores e GPUs de clientes modestos; a RTX 3050 desta máquina serve apenas para diagnóstico.

## Critérios de avanço

1. **Geração:** `port.sh` reconhece `SLUS_213.55`, traduz o código e compila o runner nativo e a página web sem erros.
2. **Boot:** runner nativo e navegador deixam a tela inicial e alcançam o menu; registrar logs e frames do mesmo trecho.
3. **Corrida:** iniciar uma corrida, controlar o carro e chegar ao fim sem travamento; medir FPS e frame time em Chrome/Edge.
4. **Áudio e saves:** música/efeitos sem cortes graves, criar e reabrir progresso no cartão de memória do navegador.
5. **Distribuição local:** servidor limitado a `127.0.0.1`, sem ISO nem WASM derivado do jogo no Git ou em hospedagem pública.

## Diagnóstico quando parar

- `games/mc3remix/explore.log`: primeira falha do runtime ou função não encontrada.
- `games/mc3remix/misses.txt` e `entries.txt`: alvos indiretos ausentes que a exploração adiciona.
- `games/mc3remix/shots/`: frames da execução nativa, para verificar onde chegou visualmente.
- `build/native-mc3remix/ninja.log`: erros de compilação nativa.
- `build/web-mc3remix/` e console do navegador: build WebAssembly e falhas de runtime web.

Tratar primeiro a causa comprovada por esses artefatos. As camadas com maior chance de trabalho específico são o driver IOP de áudio/disco, os microprogramas VU1, recursos do GS e padrões de streaming do MC3.

## Tecnologias do `nfsu2-web` usadas como referência

O [`nfsu2-web`](https://github.com/th0mk/nfsu2-web) usa Python/Capstone para elevar x86 de PC a C, wasi-sdk para WebAssembly, TypeScript/esbuild, WebGPU, AudioWorklet, workers com memória compartilhada, OPFS e servidor Rust. O método de separar jogo recompilado, runtime e host web ajuda a orientar este trabalho, mas seu código de Win32/Direct3D 9 não executa `SLUS_213.55`. [README](https://github.com/th0mk/nfsu2-web), [design](https://github.com/th0mk/nfsu2-web/blob/main/docs/web/DESIGN.md).
