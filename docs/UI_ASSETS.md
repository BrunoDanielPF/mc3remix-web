# Recursos visuais e menus internos do MC3 Remix

## Mapa confirmado nesta cópia do PS2

A ISO mantém os recursos em arquivos grandes dentro do disco. `ASSETS.DAT`, `TEXTURE.DAT` e `BANKS.DAT` usam um índice `Dave`/`DAVE`; `STREAMS.DAT` tem outro formato. O indexador [`engine/scripts/dave_index.py`](../engine/scripts/dave_index.py) lê os nomes e deslocamentos diretamente da ISO, sem copiar os arquivos de vários gigabytes. No `ASSETS.DAT` desta cópia foram identificadas 18.416 entradas.

| Recurso no `ASSETS.DAT` | Conteúdo observado | Papel provável |
| --- | --- | --- |
| `tune/ui/en/menutitlescreen.ui` | Texto com `ScreenX`, `ScreenY`, `TextZoom`, `RowHeight` e câmeras das cidades | Layout do menu principal |
| `tune/ui/en/menunetworkselect.ui` e `menunetworkhostjoin.ui` | Parâmetros de layout e câmeras | Telas originais do fluxo de rede |
| `tune/ui/en/networkarcade.ui` e `networkcustom.ui` | Layout e campos de ajuda para corridas | Configuração de eventos de rede |
| `flash/bg_main.pck` e `flash/bg_network.pck` | Pacotes binários com nomes de telas e objetos | Arte/animação e composição dos fundos |
| `fonts/mcstrings01.strtbl` | Tabela de textos em vários idiomas | Rótulos e mensagens do menu |
| `fonts/texture/*.tex` | Recursos binários de fonte | Glifos usados pelo jogo |

A tabela de textos contém rótulos originais como `CM_Networking` → “Networking”, “Host Game”, “Join Game” e “Lobby”. O formato foi conferido com o [parser de tabelas de texto de EdnessP](https://github.com/EdnessP/scripts/blob/main/midnight-club/strtbl.py), que identifica a revisão usada por MC3 Remix. A presença desses recursos **não habilita automaticamente** a opção Network no menu: o teste nativo continua mostrando seis itens. O `.ui` observado define layout, mas não contém a lista de opções clicáveis. É necessário localizar a condição e a ação no código do menu.

### Referências da opção Network no executável

O executável contém **quatro cópias** da chave `CM_Networking`, com referências em `unit_006D.cpp` (`0x003541CC`), `unit_0073.cpp` (`0x0036D900`), `unit_0074.cpp` (`0x00372440` e `0x003725C4`) e `unit_0075.cpp` (`0x00376310`). A primeira fica perto de `CM_Career` e `CM_Arcade` na seção de dados, sendo uma candidata forte para a construção do menu principal. As outras ficam perto de nomes de telas de rede e do texto `MenuOptionScreen`.

Em `unit_006D.cpp`, o bloco `0x00354150–0x003541EC` tem condições antes da chamada que usa essa chave. Um salto pode pular a entrada. **A rotina constrói o menu principal:** após restaurar o estado do quadro 3200, pressionar Start no quadro 3215 e confirmar o aviso de cartão no quadro 3610, o rastreio marcou `CM_Career` e o bloco Network no quadro **3794**. A execução anterior a partir do quadro 4600 já começava com o menu construído; por isso não registrava passagem. O valor dos bytes de estado ainda não pode ser interpretado como adaptador detectado ou configuração válida sem rastrear quem os define.

O estado salvo guarda a RAM EE a partir do deslocamento `0x5B4` do arquivo. Nas cópias de diagnóstico dos quadros 1200, 4600 e 5390, os três bytes consultados pelo bloco estavam em zero: o byte apontado pela estrutura em `0x6144F0` com deslocamento `0x293E0`, o byte global em `0x61C4A4` e o byte `+4` da estrutura apontada em `0x614430`. A ligação causal entre a condição e a ausência da opção foi confirmada pelo teste opt-in abaixo, embora a semântica de cada byte ainda precise ser apurada.

[`engine/scripts/network_menu_probe.py`](../engine/scripts/network_menu_probe.py) cria uma **árvore de código gerado isolada** com esses saltos condicionais atravessáveis sob `PS2_NETWORK_MENU_PROBE=1`. `PS2_NETWORK_MENU_TRACE=1` registra a passagem por essa rotina e por duas referências a `CM_Career`. Na rota acima, a sonda habilitada fez o menu original exibir **NETWORKING** entre Arcade e Race Editor no quadro **4200**, preservando a arte, o alinhamento e os demais itens. A mesma rota sem a sonda mostra seis opções. Com três comandos `down` e `cross`, o jogo abriu a tela original **NETWORKING** no quadro **4300**, com **Internet Play**, **LAN Play** e **Network Configuration**. Isso confirma também a ação da opção. As capturas e logs ficam apenas em `games/mc3remix/network-menu-probe/` na cópia WSL. O build comum, a ISO e a sessão em uso não foram modificados. Ainda não há conexão ou corrida multiplayer.

O build **web diagnóstico** separado em `build/web-mc3remix-menu-probe` também mostrou **NETWORKING** ao restaurar `speed-route-3200.state`, passar por Start e pelo aviso de cartão com `nocard=1`, e chegar ao quadro 3960. A sonda foi ativada por `PS2_NETWORK_MENU_PROBE=1` na URL, que o shell já repassa para o ambiente do WebAssembly. Assim, a entrada foi construída pelo código web durante o boot, não apenas carregada de um estado com a opção pronta. A aba de prévia interativa usa `network-selected-3950.state` e fica em `http://127.0.0.1:8624/`; o servidor de jogo existente em 8620 não foi alterado. A taxa de quadros observada no navegador embutido do Codex não serve como validação de desempenho para o Edge externo.

## Manipulação reversível, sem editar a ISO

O script [`engine/scripts/disc_overlay.py`](../engine/scripts/disc_overlay.py) cria um manifesto e um pequeno arquivo de substituição para **todas as cópias** de um recurso com nome exato. A substituição precisa ter o mesmo tamanho descompactado. Se o recurso for comprimido, ela também precisa caber no espaço comprimido existente; o script usa deflate bruto, preenche o restante do bloco e confirma a descompactação. O manifesto fixa o tamanho da ISO e o SHA-256 dos trechos originais. O servidor HTTP só aplica a troca se iniciado com `--disc-overlay`; o servidor e a ISO normais continuam intactos.

Exemplo no checkout WSL, usando um arquivo editado pelo usuário em uma pasta local ignorada pelo Git:

```bash
cd "$HOME/mc3remix-build"
python3 scripts/dave_index.py games/mc3remix/disc.iso ASSETS.DAT --contains menutitlescreen
python3 scripts/dave_index.py games/mc3remix/disc.iso ASSETS.DAT --extract-index 14483 --output games/mc3remix/ui-probe/title-original.ui
# Edite uma cópia do .ui, mantendo o mesmo número de bytes.
python3 scripts/disc_overlay.py games/mc3remix/disc.iso ASSETS.DAT \
  tune/ui/en/menutitlescreen.ui games/mc3remix/ui-probe/title-edited.ui \
  games/mc3remix/ui-probe/title-overlay.json
python3 scripts/serve.py build/web-mc3remix games/mc3remix/disc.iso 8622 \
  --disc-overlay games/mc3remix/ui-probe/title-overlay.json
```

Para testar várias mudanças juntas, passe `--disc-overlay` mais de uma vez, com um manifesto por recurso. O servidor confere os hashes de origem e rejeita regiões sobrepostas. Sem essa opção, o caminho de leitura da ISO permanece o de sempre.

O arquivo de prova local mudou apenas `ScreenX 60` para `ScreenX 90` em `menutitlescreen.ui`. O script encontrou as duas cópias do layout, produziu 404 bytes comprimidos para cada uma e o teste HTTP Range descompactou ambas para o texto editado. Uma leitura de trecho não modificado da ISO retornou os mesmos bytes. O jogo leu as duas posições substituídas durante um boot completo. A captura do menu não mostrou deslocamento claro; portanto, **não há prova de que `ScreenX` controle a posição dos itens visíveis** nessa tela. A posição pode vir do pacote `bg_main.pck` ou de outro campo.

Para editar um rótulo real, [`engine/scripts/strtbl_replace.py`](../engine/scripts/strtbl_replace.py) localiza uma chave na tabela de texto MC3 pelo hash e modifica somente o valor da língua escolhida, exigindo o mesmo tamanho UTF-16 e conferindo o texto antigo. A prova local substituiu `CM_Career` de `Career` por `TESTE!` apenas em uma cópia ignorada de `mcstrings01.strtbl`; o resultado foi comprimido em 222.564 bytes, dentro do bloco original de 230.848 bytes. **O menu do jogo exibiu `TESTE!` na primeira opção em um boot web isolado**, enquanto os demais itens e a arte original permaneceram visíveis. O teste foi encerrado em seguida. Esse rótulo é **diagnóstico**: a ação continuaria sendo Career. A prova demonstra manipulação visual dentro da interface, sem fingir que Multiplayer já funciona.

```bash
python3 scripts/dave_index.py games/mc3remix/disc.iso ASSETS.DAT \
  --extract-index 4976 --output games/mc3remix/ui-probe/mcstrings01.strtbl
python3 scripts/strtbl_replace.py games/mc3remix/ui-probe/mcstrings01.strtbl \
  games/mc3remix/ui-probe/mcstrings01-edited.strtbl CM_Career Career 'TESTE!'
python3 scripts/disc_overlay.py games/mc3remix/disc.iso ASSETS.DAT \
  fonts/mcstrings01.strtbl games/mc3remix/ui-probe/mcstrings01-edited.strtbl \
  games/mc3remix/ui-probe/text-overlay.json
```

Os arquivos extraídos e o manifesto de prova ficam somente em `games/mc3remix/ui-probe/`, ignorado pelo Git. Não versionar nem publicar recursos proprietários do jogo ou uma ISO modificada. Uma futura distribuição deve aplicar modificações locais à cópia do próprio jogador, respeitando a origem dos arquivos.

## Próximo ponto técnico

1. Mapear no `bg_main.pck` ou no código do menu o posicionamento dos itens visíveis; a alteração de `ScreenX` foi carregada, mas não produziu deslocamento observável. Repetir com um campo do menu de rede quando o fluxo se tornar acessível.
2. Em **LAN Play**, o quadro 4500 exige um cartão PS2 com arquivo de configuração de rede válido e oferece criar um. Ao confirmar **Yes** com uma cópia isolada do cartão, o runner recebeu `LoadExecPS2` no quadro 4829 para `cdrom0:\NTGUIDVD.ELF`. O host web diagnóstico já navega para o build separado do configurador com os argumentos de retorno preservados. Falta concluir a gravação no cartão virtual e o retorno ao jogo; também rastrear a condição que habilita Network para substituir a sonda por um estado de rede emulado correto.
3. Depois que Network for alcançável, ligar o caminho LAN do PS2 ao relay e validar duas instâncias na mesma cidade. Só então adicionar seleção de eventos de carreira como extensão, caso as regras originais não a forneçam.
4. Para novos elementos que ultrapassem o espaço do arquivo original, criar um sistema de substituição por arquivo virtual no runtime, com tamanho e índice próprios. A técnica atual é adequada a ajustes e substituições de tamanho fixo; acrescentar novos objetos `.pck` requer entender o formato e as referências antes de expandi-lo.

Referência de formato do arquivo de recursos: [parser DAVE de EdnessP](https://github.com/EdnessP/scripts/blob/main/midnight-club/dave.py).
