<div align="center">

<img src="wheel-filter.svg" width="112" alt="Wheel Filter">

# Wheel Filter

**Acaba com os "ticks fantasma" da roda do mouse.**
Aquele momento em que você rola para baixo e a página dá um pulinho para cima.

![C](https://img.shields.io/badge/C-blue) ![GTK4](https://img.shields.io/badge/GTK-4-green) ![libadwaita](https://img.shields.io/badge/libadwaita-GNOME-purple) ![Linux](https://img.shields.io/badge/Linux-evdev%20%2F%20uinput-lightgrey)

</div>

---

## O problema

Em alguns mouses, a roda com o encoder gasto ou sujo emite **inversões espúrias**: no meio de uma rolagem para baixo surge um tick isolado para cima. O resultado é a página tremendo, o PDF voltando uma linha, o scroll "engasgando".

O Wheel Filter fica entre o mouse e o sistema. Ele captura o mouse, decide tick a tick o que é rolagem de verdade e o que é ghost, e recria um mouse virtual que só entrega a rolagem limpa. O resto do mouse (movimento, botões) passa intacto.

```
 mouse físico ──► evdev ──► [ filtro ] ──► uinput ──► mouse virtual ──► sistema
                              │
                              └── aprende com o seu uso
```

## Recursos

- **Filtro de inversões por confirmação.** Uma inversão de direção só vale se vier confirmada por N ticks seguidos na nova direção. Se a direção antiga voltar enquanto a inversão espera, ela é descartada.
- **Rolagem contínua protegida.** No meio de uma rolagem longa (ler um PDF, uma página extensa), uma inversão rápida demais para um humano exige mais confirmações e, se ficar isolada, cai.
- **Aprendizado enquanto você usa.**
  - Descobre o menor tempo que *você* leva para inverter a roda de verdade.
  - Rotula ghosts que passaram quando a direção antiga volta logo depois, e corrige a posição estimada da roda.
  - Mapeia em quais **setores do leito da roda** (detentes) o ghost se concentra e fica mais rigoroso ali. A taxa por setor usa estimativa bayesiana e esquece o passado antigo, acompanhando o desgaste.
  - Salva o que aprendeu em disco, um arquivo por mouse, e realinha a tabela na próxima sessão.
- **Interface GTK4 + libadwaita.** Segue o tema claro/escuro do sistema, mostra estatísticas ao vivo, o anel dos setores e um monitor da roda.
- **Fica na bandeja.** Fechar a janela no "X" não encerra o filtro: ele continua em segundo plano, com ícone na bandeja e menu (clique direito) para escolher o dispositivo e os parâmetros. *(No GNOME é preciso a extensão AppIndicator/KStatusNotifierItem.)*
- **Serviço systemd** para filtrar desde o boot, e reconexão automática se o mouse sumir e voltar.
- **Modo replay** para analisar offline um log do `evtest`.

## Instalação

### Dependências

| | Arch / Manjaro | Debian / Ubuntu |
|---|---|---|
| Compilador | `base-devel` | `build-essential` |
| Interface | `gtk4 libadwaita libdbusmenu-glib` | `libgtk-4-dev libadwaita-1-dev libdbusmenu-glib-dev` |
| Elevação | `polkit` | `policykit-1` |

Sem as bibliotecas da interface, o `make` compila só o modo linha de comando.

### Compilar e instalar

```sh
make            # compila
make test       # roda os testes do filtro
sudo make install
```

A instalação coloca o binário em `/usr/local/bin`, instala e **ativa o serviço** `wheel-filter`, carrega o módulo `uinput`, e adiciona o atalho no menu de aplicativos (com ícone) e a política do polkit. Use `PREFIX=/usr` para outro destino.

> O serviço instalado usa `-d G703` (Logitech G703). Para outro mouse, edite `wheel-filter.service` antes de instalar.

### Desinstalar

```sh
sudo make uninstall
```

Para e desabilita o serviço, encerra qualquer motor aberto pela interface e remove todos os arquivos.

## Uso

### Interface gráfica

```sh
wheel-filter --gui
```

ou abra **Wheel Filter** pelo menu de aplicativos. Escolha o mouse, ajuste os parâmetros se quiser e clique em **Iniciar filtro**. O app pede autenticação (polkit) porque precisa capturar o dispositivo.

> Se o serviço `wheel-filter` já estiver ativo, ele já captura o mouse. Pare-o antes (`sudo systemctl stop wheel-filter`) para usar o filtro pela interface.

Os parâmetros:

| Parâmetro | Padrão | O que faz |
|---|---|---|
| Ticks para confirmar | 2 | Ticks seguidos na nova direção para aceitar uma inversão. Mais alto filtra mais, mas atrasa inversões reais. `1` desliga a filtragem por confirmação. |
| Tempo de confirmação | 350 ms | Uma inversão isolada que não for seguida de nada nesse tempo é considerada real. |
| Inatividade | 500 ms | Depois desse tempo sem eventos, o próximo tick passa direto. |
| Modo observação | desligado | Só registra os ghosts, sem capturar o mouse. Bom para diagnosticar. |

### Linha de comando

```sh
sudo wheel-filter -d G703                 # ao vivo, achando o mouse pelo nome
sudo wheel-filter /dev/input/eventN       # ao vivo, por caminho
wheel-filter log.txt                      # replay de um log do evtest
```

| Opção | Descrição |
|---|---|
| `-c N` | ticks para confirmar uma inversão (padrão 2) |
| `-f ms` | tempo para considerar uma inversão isolada como real (350) |
| `-i ms` | inatividade após a qual o próximo tick sempre passa (500) |
| `-d nome` | acha o mouse pelo nome; espera se ele sumir |
| `-n` | só observar: não captura nem filtra |
| `-l arq` | registra os ticks descartados em um arquivo |
| `-s dir` | onde guardar o que o filtro aprendeu |

O que o filtro aprendeu fica em `/var/lib/wheel-filter` (como root) ou `~/.local/state/wheel-filter`.

## Como funciona

O motor (`filter.c`) não faz nenhuma E/S: recebe ticks e devolve vereditos, o que o torna fácil de testar.

1. **Confirmação.** Um tick na direção oposta à última aceita fica em espera. Se a direção antiga voltar, a espera é descartada como ruído. Se o número de ticks necessários for atingido, a inversão passa.
2. **Rolagem contínua.** Quatro ou mais ticks seguidos em ritmo curto formam uma rolagem contínua. Nela, uma inversão mais rápida que cerca de 4× o seu intervalo típico entre ticks é suspeita: precisa de confirmações extras e, se ficar sozinha, é descartada.
3. **Setores.** A posição da roda é integrada a partir dos ticks aceitos e dividida em detentes (período detectado automaticamente entre 16, 18, 20, 24, 27 e 36). Setores com muito ghost exigem mais confirmação.
4. **Pseudo-rótulos.** Uma inversão aceita que é desfeita em menos de 0,5 s era um ghost que passou: o filtro conta isso no setor e corrige a posição. Inversões confirmadas por 3 ou mais ticks ensinam o piso humano de inversão.
5. **Persistência.** Como a roda começa em uma posição arbitrária a cada sessão, a tabela salva é girada até casar com os primeiros ghosts da sessão nova.

Os arquivos:

| Arquivo | Papel |
|---|---|
| `filter.c/h` | motor do filtro e aprendizado |
| `io.c/h` | sessão ao vivo: evdev, uinput, reconexão, persistência |
| `devices.c/h` | listagem de mouses |
| `gui.c/h` | interface GTK4 + libadwaita |
| `tray.c/h` | ícone e menu da bandeja (StatusNotifierItem) |
| `main.c` | linha de comando, replay e modo `--engine` da interface |
| `test_filter.c` | testes do filtro |

## Desenvolvimento

```sh
make test          # testes do motor, sem precisar de mouse
make               # binário em ./wheel-filter
```

Para calibrar com o seu mouse, rode em modo observação gravando os descartes (`-n -l ghosts.log`), ou capture um log com `evtest` e use o replay.

## Limitações

- Precisa de privilégios para capturar o mouse e criar o dispositivo virtual (por isso o polkit e o serviço).
- O ícone da bandeja no GNOME depende da extensão AppIndicator/KStatusNotifierItem. Sem ela o app continua rodando, só sem ícone.
- Os limiares do modo de rolagem contínua são valores iniciais razoáveis; podem pedir ajuste conforme o seu mouse.
- Desenvolvido em torno de um Logitech G703 (o padrão do serviço); outros mouses devem funcionar, mas o período de detentes e os tempos podem variar.
